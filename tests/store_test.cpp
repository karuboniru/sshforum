#include "sshforum/store.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Exception, typename Function>
void require_throws(Function&& function, const std::string& message) {
    try {
        function();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(message);
}

class TemporaryDatabase {
public:
    TemporaryDatabase() {
        auto filename = (std::filesystem::temp_directory_path() / "sshforum-store-XXXXXX").string();
        std::vector<char> buffer(filename.begin(), filename.end());
        buffer.push_back('\0');
        const int descriptor = mkstemp(buffer.data());
        if (descriptor < 0) {
            throw std::runtime_error("could not create a temporary database path");
        }
        close(descriptor);
        path_ = buffer.data();
    }

    ~TemporaryDatabase() {
        std::error_code error;
        std::filesystem::remove(path_, error);
        std::filesystem::remove(path_.string() + "-wal", error);
        std::filesystem::remove(path_.string() + "-shm", error);
    }

    TemporaryDatabase(const TemporaryDatabase&) = delete;
    TemporaryDatabase& operator=(const TemporaryDatabase&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

void test_crud_and_persistence() {
    TemporaryDatabase database;
    std::int64_t thread_id;
    std::int64_t reply_id;

    {
        sshforum::Store store(database.path().string());
        require(store.list_threads().empty(), "a new database should have no threads");
        require(!store.get_thread(123456), "a missing thread should not be found");

        thread_id = store.create_thread("First topic", "Opening post");
        require(thread_id > 0, "created threads should have positive IDs");
        auto thread = store.get_thread(thread_id);
        require(thread.has_value(), "the created thread should be readable");
        require(thread->summary.id == thread_id, "thread ID should be preserved");
        require(thread->summary.title == "First topic", "thread title should be preserved");
        require(thread->summary.body == "Opening post", "opening post should be preserved");
        require(!thread->summary.created_at.empty(), "thread timestamp should be populated");
        require(thread->summary.reply_count == 0, "a new thread should have no replies");
        require(thread->replies.empty(), "a new thread should have an empty reply list");

        reply_id = store.reply(thread_id, "First reply");
        require(reply_id > 0, "created replies should have positive IDs");
        thread = store.get_thread(thread_id);
        require(thread->summary.reply_count == 1, "reply count should update");
        require(thread->replies.size() == 1, "the reply should be returned");
        require(thread->replies.front().id == reply_id, "reply ID should be preserved");
        require(thread->replies.front().thread_id == thread_id, "reply should belong to its thread");
        require(thread->replies.front().body == "First reply", "reply body should be preserved");
        require(!thread->replies.front().created_at.empty(), "reply timestamp should be populated");
    }

    {
        sshforum::Store reopened(database.path().string());
        auto thread = reopened.get_thread(thread_id);
        require(thread.has_value(), "thread should survive reopening the database");
        require(thread->summary.title == "First topic", "reopened thread title should match");
        require(thread->summary.body == "Opening post", "reopened opening post should match");
        require(thread->summary.reply_count == 1, "reopened reply count should match");
        require(thread->replies.size() == 1 && thread->replies.front().id == reply_id,
                "reply should survive reopening the database");
        require(reopened.list_threads().size() == 1, "listing should survive reopening the database");
    }
}

void test_validation_and_missing_thread() {
    TemporaryDatabase database;
    sshforum::Store store(database.path().string());

    require_throws<std::invalid_argument>([&] { store.create_thread("", "body"); },
                                          "empty titles should be rejected");
    require_throws<std::invalid_argument>([&] { store.create_thread(" \t\n", "body"); },
                                          "whitespace titles should be rejected");
    require_throws<std::invalid_argument>([&] { store.create_thread("title", ""); },
                                          "empty opening posts should be rejected");
    require_throws<std::invalid_argument>([&] { store.create_thread("title", " \t\n"); },
                                          "whitespace opening posts should be rejected");
    require_throws<std::invalid_argument>([&] { store.create_thread(std::string(100000, 'x'), "body"); },
                                          "oversized titles should be rejected");
    require_throws<std::invalid_argument>([&] { store.create_thread("title", std::string(1000000, 'x')); },
                                          "oversized opening posts should be rejected");
    require_throws<std::invalid_argument>([&] { store.list_threads(0); },
                                          "zero list limit should be rejected");
    require(store.list_threads().empty(), "invalid thread creation should not leave a row");

    const auto thread_id = store.create_thread("Valid", "Opening post");
    require_throws<std::invalid_argument>([&] { store.reply(thread_id, ""); },
                                          "empty replies should be rejected");
    require_throws<std::invalid_argument>([&] { store.reply(thread_id, " \t\n"); },
                                          "whitespace replies should be rejected");
    require_throws<std::invalid_argument>([&] { store.reply(thread_id, std::string(1000000, 'x')); },
                                          "oversized replies should be rejected");
    require_throws<std::out_of_range>([&] { store.reply(thread_id + 1000, "orphan"); },
                                      "replies to missing threads should be rejected");
    require(!store.get_thread(thread_id + 1000), "a missing thread should remain absent");
    require(store.get_thread(thread_id)->replies.empty(), "invalid replies should not leave rows");
}

void test_sql_metacharacters() {
    TemporaryDatabase database;
    sshforum::Store store(database.path().string());
    const std::string title = "Robert'); DROP TABLE threads; --";
    const std::string body = "' OR 1=1; -- \"quoted\"";
    const std::string reply = "'); DELETE FROM posts; --";

    const auto first_id = store.create_thread(title, body);
    store.reply(first_id, reply);
    const auto second_id = store.create_thread("Still here", "Another post");
    auto first = store.get_thread(first_id);
    require(first.has_value(), "thread with SQL metacharacters should be readable");
    require(first->summary.title == title && first->summary.body == body,
            "SQL metacharacters should be stored verbatim");
    require(first->replies.size() == 1 && first->replies.front().body == reply,
            "reply SQL metacharacters should be stored verbatim");
    require(store.get_thread(second_id).has_value(), "SQL metacharacters should not affect other rows");
    require(store.list_threads().size() == 2, "SQL metacharacters should not affect the schema");
}

void test_latest_reply_sorting_and_limit() {
    TemporaryDatabase database;
    sshforum::Store store(database.path().string());
    const auto first_id = store.create_thread("Older", "First post");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    const auto second_id = store.create_thread("Newer", "Second post");

    auto threads = store.list_threads();
    require(threads.size() == 2, "both threads should appear in the listing");
    require(threads.front().id == second_id, "newer threads should sort first");

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    store.reply(first_id, "Bump older topic");
    threads = store.list_threads();
    require(threads.front().id == first_id, "the most recently replied thread should sort first");
    require(threads.front().reply_count == 1, "listing should include the updated reply count");

    threads = store.list_threads(1);
    require(threads.size() == 1 && threads.front().id == first_id,
            "list limit should return the first thread in sort order");
}

}  // namespace

int main() {
    try {
        test_crud_and_persistence();
        test_validation_and_missing_thread();
        test_sql_metacharacters();
        test_latest_reply_sorting_and_limit();
        std::cout << "store tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "store tests failed: " << error.what() << '\n';
        return 1;
    }
}
