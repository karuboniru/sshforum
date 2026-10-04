#include "sshforum/store.hpp"
#include "sshforum/tui.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class TemporaryDatabase {
public:
    TemporaryDatabase() {
        std::string pattern = (std::filesystem::temp_directory_path() /
                               "sshforum-tui-XXXXXX").string();
        std::vector<char> path(pattern.begin(), pattern.end());
        path.push_back('\0');
        const int descriptor = mkstemp(path.data());
        if (descriptor < 0) throw std::runtime_error("mkstemp failed");
        close(descriptor);
        path_ = path.data();
    }

    ~TemporaryDatabase() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(path_ + "-wal", ignored);
        std::filesystem::remove(path_ + "-shm", ignored);
    }

    const std::string& path() const { return path_; }

private:
    std::string path_;
};

void test_post_reply_and_utf8() {
    TemporaryDatabase database;
    sshforum::Store store(database.path());
    sshforum::Tui tui(store);
    tui.resize(40, 12);
    require(tui.start().find("\x1b[?1049h\x1b[?25l") == 0,
            "start should enter alternate screen and hide cursor");

    tui.input("n");
    tui.input("\xe9");
    tui.input("\xa2\x98"); // 题, split across network reads.
    tui.input("\xe9\x94\x99"); // 错
    tui.input("\x7f"); // Erase one whole UTF-8 character.
    tui.input("\r\n");
    tui.input("\xe4\xbd");
    tui.input("\xa0\xe5\xa5\xbd"); // 你好, split UTF-8.
    tui.input("\rsecond line");
    const auto posted = tui.input("\x04");
    const auto listed = store.list_threads();
    require(listed.size() == 1, "Ctrl+D should create a thread");
    require(listed.front().title == "题", "backspace should remove one UTF-8 character");
    require(listed.front().body == "你好\nsecond line", "body should preserve UTF-8 and newline");
    require(posted.find("你好") != std::string::npos, "created thread should be visible");
    require(posted.find("Anonymous") != std::string::npos,
            "thread author should be anonymous");

    tui.input("a");
    tui.input("回帖");
    const auto replied = tui.input("\x04");
    auto thread = store.get_thread(listed.front().id);
    require(thread && thread->replies.size() == 1, "Ctrl+D should create a reply");
    require(thread->replies.front().body == "回帖", "reply should preserve UTF-8");
    require(replied.find("回帖") != std::string::npos, "reply should be visible after submit");
    require(replied.find("Anonymous") != std::string::npos,
            "reply author should be anonymous");

    tui.input("b");
    const auto resized = (tui.resize(18, 5), tui.input(""));
    require(resized.find("\x1b[H\x1b[2J") == 0, "resize should redraw on empty input");
    require(resized.find("\x1b[31m") == std::string::npos, "screen should not contain input styling");
    require(tui.input("q") == sshforum::Tui::restore_terminal(),
            "quit should restore terminal");
    require(tui.done(), "quit should mark TUI done");
}

void test_escape_keys_and_sanitization() {
    TemporaryDatabase database;
    sshforum::Store store(database.path());
    const auto first = store.create_thread("first", "body one");
    const auto second = store.create_thread("second", "body two");
    sshforum::Tui tui(store);
    tui.start();
    tui.input("\x1b");
    tui.input("[");
    const auto moved = tui.input("B");
    require(moved.find("> first") != std::string::npos ||
            moved.find("> second") != std::string::npos,
            "split down arrow should select a thread");
    const auto opened = tui.input("\r");
    require(opened.find("Thread #" + std::to_string(first)) != std::string::npos ||
            opened.find("Thread #" + std::to_string(second)) != std::string::npos,
            "Enter should open selected thread");

    tui.input("b");
    tui.input("n");
    tui.input("safe\x1b[31m title");
    tui.input("\xc2\x9b"); // Unicode C1 CSI must not reach the screen or database.
    tui.input("\rbody\x1b[2J text");
    auto screen = tui.input("\x04");
    require(screen.find("\x1b[31m") == std::string::npos &&
            screen.find("\x1b[2J text") == std::string::npos,
            "user input must not execute ANSI sequences");
    auto latest = store.list_threads().front();
    require(latest.title.find('\x1b') == std::string::npos &&
            latest.body.find('\x1b') == std::string::npos,
            "ANSI escape bytes must not be stored");

    tui.input("a");
    tui.input("discard");
    tui.input("\x1b");
    std::this_thread::sleep_for(std::chrono::milliseconds(90));
    screen = tui.input("");
    require(screen.find("Cancelled") != std::string::npos,
            "lone Escape should cancel editing after idle tick");
    require(store.get_thread(latest.id)->replies.empty(), "cancel should not save reply");
    require(tui.input("\x03") == sshforum::Tui::restore_terminal(),
            "Ctrl+C should restore terminal");
}

void test_limits_and_paging() {
    TemporaryDatabase database;
    sshforum::Store store(database.path());
    for (int i = 0; i < 6; ++i)
        store.create_thread("topic " + std::to_string(i), "body");
    sshforum::Tui tui(store);
    tui.resize(50, 5);
    tui.start();
    const auto paged = tui.input("\x1b[6~");
    require(paged.find("> topic 2") != std::string::npos,
            "PageDown should move one visible page in list");
    const auto reset = tui.input("\x1b[5~");
    require(reset.find("> topic 5") != std::string::npos,
            "PageUp should return to first thread");

    tui.input("n");
    tui.input(std::string(120, 'x'));
    require(tui.input("z").find("Title limit: 120 bytes") != std::string::npos,
            "title overflow should show limit");
    tui.input("\r");
    tui.input(std::string(16384, 'y'));
    require(tui.input("z").find("Body limit: 16384 bytes") != std::string::npos,
            "body overflow should show limit");
    tui.input("\x04");
    auto latest = store.list_threads().front();
    require(latest.title.size() == 120 && latest.body.size() == 16384,
            "editor should enforce Store byte limits");
}

} // namespace

int main() {
    test_post_reply_and_utf8();
    test_escape_keys_and_sanitization();
    test_limits_and_paging();
}
