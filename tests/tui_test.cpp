#include "sshforum/store.hpp"
#include "sshforum/tui.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::size_t count_occurrences(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size())) ++count;
    return count;
}

void execute_sql(const std::string& path, const std::string& sql) {
    sqlite3* database = nullptr;
    if (sqlite3_open(path.c_str(), &database) != SQLITE_OK) {
        const std::string message = database ? sqlite3_errmsg(database) : "open failed";
        sqlite3_close(database);
        throw std::runtime_error(message);
    }
    char* error = nullptr;
    const int result = sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &error);
    if (result != SQLITE_OK) {
        const std::string message = error ? error : sqlite3_errmsg(database);
        sqlite3_free(error);
        sqlite3_close(database);
        throw std::runtime_error(message);
    }
    sqlite3_close(database);
}

std::vector<std::string> screen_rows(const std::string& screen) {
    constexpr std::string_view clear = "\x1b[H\x1b[2J";
    const auto begin = screen.find(clear);
    require(begin != std::string::npos, "screen should contain a clear sequence");
    std::vector<std::string> rows;
    std::size_t at = begin + clear.size();
    for (;;) {
        const auto end = screen.find("\x1b[K", at);
        require(end != std::string::npos, "every screen row should clear its remainder");
        rows.push_back(screen.substr(at, end - at));
        at = end + 3;
        if (screen.compare(at, 2, "\r\n") != 0) break;
        at += 2;
    }
    return rows;
}

std::size_t find_row(const std::vector<std::string>& rows, const std::string& needle) {
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (rows[i].find(needle) != std::string::npos) return i;
    throw std::runtime_error("screen row missing: " + needle);
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

void test_stable_author_identity() {
    TemporaryDatabase database;
    sshforum::Store store(database.path());
    const std::string self_id = "v1:" + std::string(64, '0');
    const std::string other_id = "v1:" + std::string(64, 'f');
    const auto historical_id = store.create_thread("historical", "old body");
    const auto other_thread_id = store.create_thread("other", "other body", other_id);
    store.reply(other_thread_id, "other reply", other_id);

    sshforum::Tui tui(store);
    tui.set_author_id(self_id);
    tui.resize(100, 12);
    const auto listing = tui.start();
    require(listing.find("You: Anonymous#AAAAAAAA") != std::string::npos,
            "thread list should identify the current anonymous author");
    require(listing.find(self_id) == std::string::npos,
            "thread list must not show the raw author ID");

    const auto other_screen = tui.input("\r");
    require(count_occurrences(other_screen, "Anonymous#77777777") == 2 &&
            other_screen.find("other reply") != std::string::npos,
            "thread and reply should show their stored author's identity");
    require(other_screen.find("Anonymous#AAAAAAAA") == std::string::npos,
            "another author's posts must not use the current session identity");
    require(other_screen.find(other_id) == std::string::npos,
            "thread screen must not show a raw author ID");

    tui.input("b");
    tui.input("j");
    const auto historical_screen = tui.input("\r");
    require(historical_screen.find("historical") != std::string::npos &&
            historical_screen.find("Anonymous") != std::string::npos &&
            historical_screen.find("Anonymous#") == std::string::npos,
            "posts with empty legacy author IDs should remain Anonymous");
    require(store.get_thread(historical_id)->summary.author_id.empty(),
            "historical post should retain its empty author ID");

    tui.input("b");
    tui.input("n");
    tui.input("mine\rmy body");
    const auto posted = tui.input("\x04");
    const auto latest = store.list_threads().front();
    require(latest.title == "mine" && latest.author_id == self_id,
            "new threads should persist the current session author ID");
    require(posted.find("Anonymous#AAAAAAAA") != std::string::npos &&
            posted.find(self_id) == std::string::npos,
            "new thread should show only the derived anonymous label");

    tui.input("a");
    tui.input("my reply");
    const auto replied = tui.input("\x04");
    const auto thread = store.get_thread(latest.id);
    require(thread && thread->replies.size() == 1 &&
            thread->replies.front().author_id == self_id,
            "new replies should persist the current session author ID");
    require(count_occurrences(replied, "Anonymous#AAAAAAAA") == 2 &&
            replied.find("my reply") != std::string::npos &&
            replied.find(self_id) == std::string::npos,
            "new reply should show only the derived anonymous label");
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
    require(paged.find("> topic 4") != std::string::npos,
            "PageDown should move one visible page in list");
    const auto reset = tui.input("\x1b[5~");
    require(reset.find("> topic 5") != std::string::npos,
            "PageUp should return to first thread");

    tui.resize(30, 3); // Only one content row fits; navigation must still work.
    const auto tiny = tui.input("j");
    require(tiny.find("topic 4") != std::string::npos,
            "a one-row viewport should still show the selected thread");
    require(tui.input("\r").find("Thread #") != std::string::npos,
            "a thread should remain openable from a one-row viewport");
    tui.input("b");
    tui.resize(50, 5);

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

void test_list_relative_times_and_alignment() {
    TemporaryDatabase database;
    sshforum::Store store(database.path());
    const auto no_reply_id = store.create_thread("Recent topic", "recent body");
    const auto replied_id = store.create_thread("Older topic", "older body");
    store.reply(replied_id, "older reply");
    execute_sql(database.path(),
        "UPDATE threads SET created_at = "
        "strftime('%Y-%m-%dT%H:%M:%fZ', 'now', '-2 hours', '-30 minutes', '-20 seconds') "
        "WHERE id = " + std::to_string(no_reply_id));
    execute_sql(database.path(),
        "UPDATE threads SET created_at = "
        "strftime('%Y-%m-%dT%H:%M:%fZ', 'now', '-3 days', '-2 hours') "
        "WHERE id = " + std::to_string(replied_id));
    execute_sql(database.path(),
        "UPDATE posts SET created_at = "
        "strftime('%Y-%m-%dT%H:%M:%fZ', 'now', '-1 day', '-5 hours') "
        "WHERE thread_id = " + std::to_string(replied_id));

    sshforum::Tui tui(store);
    tui.resize(60, 8);
    const auto rows = screen_rows(tui.start());
    const auto recent = find_row(rows, "Recent topic");
    const auto older = find_row(rows, "Older topic");
    require(recent + 1 < rows.size() && older + 1 < rows.size(),
            "each list item should have two visible rows");
    const auto first = std::min(recent, older);
    const auto second = std::max(recent, older);
    require(second == first + 3 && rows[first + 2].empty(),
            "one blank row should separate list entries");
    require(rows[recent].starts_with("  Recent topic") &&
            rows[recent].ends_with("Posted: 2h 30m ago") &&
            rows[recent].size() == 59,
            "recent thread title and posted age should occupy opposite ends of the first row");
    require(rows[recent + 1].starts_with("  [0 replies]") &&
            rows[recent + 1].ends_with("Last reply: No replies yet") &&
            rows[recent + 1].size() == 59,
            "a thread without replies should show its count and last-reply text on one row");
    require(rows[older].find("Older topic") != std::string::npos &&
            rows[older].ends_with("Posted: 3d ago") && rows[older].size() == 59,
            "posted ages of at least 24 hours should use days on the right edge");
    require(rows[older + 1].starts_with("  [1 replies]") &&
            rows[older + 1].ends_with("Last reply: 1d ago") &&
            rows[older + 1].size() == 59,
            "the second row should use the latest reply's day age");
}

void test_message_separators() {
    TemporaryDatabase database;
    sshforum::Store store(database.path());
    store.create_thread("Separation", "original message");
    const auto id = store.list_threads().front().id;
    store.reply(id, "first response");
    store.reply(id, "second response");

    sshforum::Tui tui(store);
    tui.resize(32, 30);
    tui.start();
    const auto rows = screen_rows(tui.input("\r"));
    const auto original = find_row(rows, "original message");
    const auto first = find_row(rows, "first response");
    const auto second = find_row(rows, "second response");
    const std::string separator(31, '-');
    std::vector<std::size_t> separators;
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (rows[i] == separator) separators.push_back(i);
    require(separators.size() == 2 &&
            original < separators[0] && separators[0] < first &&
            first < separators[1] && separators[1] < second,
            "a full content-width separator should appear between each pair of messages");
}

void test_cursor_editing_and_reply() {
    TemporaryDatabase database;
    sshforum::Store store(database.path());
    sshforum::Tui tui(store);
    tui.resize(24, 8);
    tui.start();
    require(tui.input("n").find("\x1b[2;1H\x1b[?25h") != std::string::npos,
            "editor should show the real cursor at the insertion point");
    tui.input("甲乙丙");
    tui.input("\x1bOD\x1b[D"); // SS3 and CSI left arrows.
    tui.input("中");
    tui.input("\x1b[3~"); // Delete 乙 to the right.
    tui.input("\x7f"); // Backspace 中 to the left.
    tui.input("\x1b[H头\x1b[F尾");
    tui.input("\r");
    tui.input("甲a\n12345\n乙b");
    tui.input("\x1b[A"); // Keep display column 3, before '4'.
    tui.input("X");
    tui.input("\x1b[1;5H"); // Beginning of body.
    tui.input("前");
    tui.input("\x1b[1;5F"); // End of body.
    tui.input("后");
    tui.input("\x04");
    const auto listed = store.list_threads();
    require(listed.size() == 1 && listed.front().title == "头甲丙尾",
            "middle insert, Delete, Backspace, Home and End should edit title by codepoint");
    require(listed.front().body == "前甲a\n123X45\n乙b后",
            "up arrow should follow display cells and Ctrl+Home/End should reach body edges");

    tui.input("a");
    tui.input("甲乙\n尾");
    tui.input("\x1b[A"); // From display column 2 to between 甲 and 乙.
    tui.input("中");
    tui.input("\x1b[3~");
    tui.input("\x04");
    const auto thread = store.get_thread(listed.front().id);
    require(thread && thread->replies.size() == 1 &&
            thread->replies.front().body == "甲中\n尾",
            "reply editor should share Unicode cursor editing and Delete behavior");
    require(tui.input("b").find("\x1b[?25l") != std::string::npos,
            "browser should hide the terminal cursor");
}

void test_wrap_viewport_and_resize() {
    TemporaryDatabase database;
    sshforum::Store store(database.path());
    sshforum::Tui tui(store);
    tui.resize(6, 4); // Five editable cells and two visible editor rows.
    tui.start();
    tui.input("nT\r");
    tui.input("abcdef");
    auto screen = tui.input("\x1b[H"); // Home on second visual row.
    require(screen.find("\x1b[3;1H\x1b[?25h") != std::string::npos,
            "Home should move to the start of a wrapped display row");
    screen = tui.input("\x1b[A\x1b[F"); // Up, then End of first visual row.
    require(screen.find("\x1b[2;6H\x1b[?25h") != std::string::npos,
            "End should reach the wrap boundary without moving to the next row");
    screen = tui.input("\x1b[C");
    require(screen.find("\x1b[3;2H\x1b[?25h") != std::string::npos,
            "right arrow should advance one codepoint across the wrap");
    screen = tui.input("ghijk");
    require(screen.find("fghij") != std::string::npos &&
            screen.find("abcde") == std::string::npos &&
            screen.find("\x1b[3;2H\x1b[?25h") != std::string::npos,
            "viewport should follow the cursor through soft wraps");
    screen = tui.input("\x1b[1;5H");
    require(screen.find("abcde") != std::string::npos &&
            screen.find("\x1b[2;1H\x1b[?25h") != std::string::npos,
            "Ctrl+Home should scroll the viewport back to the first row");
    tui.resize(10, 5);
    screen = tui.input("");
    require(screen.find("\x1b[2;1H\x1b[?25h") != std::string::npos,
            "resize should keep the editor cursor visible");
    tui.input("\x04");
    require(store.list_threads().front().body == "abcdefghijk",
            "navigation and viewport should not alter the draft");
}

void test_middle_insertion_byte_limits() {
    TemporaryDatabase database;
    sshforum::Store store(database.path());
    sshforum::Tui tui(store);
    tui.start();
    tui.input("n");
    tui.input(std::string(119, 't'));
    tui.input("\x1b[1;5H");
    require(tui.input("中").find("Title limit: 120 bytes") != std::string::npos,
            "UTF-8 insertion must count title bytes at the cursor");
    tui.input("x");
    require(tui.input("\x1b[F").find("\x1b[2;80H\x1b[?25h") != std::string::npos,
            "End should locate the last cell of a long title's visual row");
    tui.input("\x1b[1;5F");
    tui.resize(20, 5);
    require(tui.input("").find("\x1b[4;7H\x1b[?25h") != std::string::npos,
            "resizing a long title should keep its cursor in the visible viewport");
    tui.input("\r");
    tui.resize(50, 5);
    tui.input(std::string(16382, 'b'));
    tui.input("\x1b[1;5H");
    require(tui.input("中").find("Body limit: 16384 bytes") != std::string::npos,
            "UTF-8 insertion must count body bytes at the cursor");
    tui.input("x\n");
    require(tui.input("y").find("Body limit: 16384 bytes") != std::string::npos,
            "newline inserted in the middle must use one byte of body limit");
    tui.input("\x04");
    const auto latest = store.list_threads().front();
    require(latest.title.size() == 120 && latest.title.front() == 'x',
            "title insertion at byte limit should preserve the cursor position");
    require(latest.body.size() == 16384 && latest.body.starts_with("x\n"),
            "body insertion at byte limit should preserve UTF-8 boundaries");
}

void test_bounded_input_and_parser_state() {
    TemporaryDatabase database;
    sshforum::Store store(database.path());
    sshforum::Tui tui(store);
    tui.start();

    const std::string commands = std::string(100, 'r') + "n";
    auto result = tui.input_some(commands);
    require(result.consumed > 0 && result.consumed <= 32,
            "a bounded call should process at most 32 refresh commands");
    require(result.output.find("SSH Forum | Threads") != std::string::npos,
            "a bounded refresh batch should render its final list state");
    std::size_t consumed = result.consumed;
    while (consumed < commands.size()) {
        result = tui.input_some(std::string_view(commands).substr(consumed));
        require(result.consumed > 0 && result.consumed <= 32,
                "each bounded call should make progress without passing the command budget");
        consumed += result.consumed;
    }
    require(result.output.find("New thread | Title") != std::string::npos,
            "unconsumed input should resume at the exact command boundary");

    const auto feed = [&](std::string_view bytes) {
        std::string output;
        for (std::size_t at = 0; at < bytes.size();) {
            const auto part = tui.input_some(bytes.substr(at));
            require(part.consumed > 0, "bounded input should make progress on nonempty input");
            at += part.consumed;
            if (!part.output.empty()) output = part.output;
        }
        return output;
    };
    feed("T\r");
    feed("a\xe4");
    feed("\xb8");
    feed("\xad" "b");
    feed("\x1b");
    feed("[");
    feed("D");
    feed("X\x1b[C\r");
    feed("\n" "z");
    const auto submitted = feed("\x04");
    require(submitted.find("a中Xb") != std::string::npos,
            "bounded input should render the submitted draft");
    const auto latest = store.list_threads().front();
    require(latest.title == "T" && latest.body == "a中Xb\nz",
            "bounded calls should preserve split UTF-8, Escape, and CRLF state");

    feed("a");
    feed("draft");
    feed("\x1b");
    std::this_thread::sleep_for(std::chrono::milliseconds(90));
    require(tui.input_some("").output.find("Cancelled") != std::string::npos,
            "an empty bounded call should resolve a timed-out lone Escape");
    const std::string exit_commands = "bqextra";
    std::size_t exit_consumed = 0;
    std::string exit_output;
    while (exit_consumed < exit_commands.size() && !tui.done()) {
        const auto part = tui.input_some(std::string_view(exit_commands).substr(exit_consumed));
        require(part.consumed > 0, "bounded input should progress toward quit");
        exit_consumed += part.consumed;
        if (!part.output.empty()) exit_output = part.output;
    }
    require(exit_consumed == 2 && exit_output == sshforum::Tui::restore_terminal(),
            "bounded input should stop exactly at the quit command");
}

} // namespace

int main() {
    test_post_reply_and_utf8();
    test_stable_author_identity();
    test_escape_keys_and_sanitization();
    test_limits_and_paging();
    test_list_relative_times_and_alignment();
    test_message_separators();
    test_cursor_editing_and_reply();
    test_wrap_viewport_and_resize();
    test_middle_insertion_byte_limits();
    test_bounded_input_and_parser_state();
}
