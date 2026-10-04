#include "sshforum/store.hpp"

#include <sqlite3.h>

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

class RawDatabase {
public:
    explicit RawDatabase(const std::filesystem::path& path) {
        if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
            const std::string message = sqlite3_errmsg(db_);
            sqlite3_close(db_);
            throw std::runtime_error("open raw database: " + message);
        }
    }

    ~RawDatabase() { sqlite3_close(db_); }

    void execute(const char* sql) {
        char* error = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string message = error != nullptr ? error : sqlite3_errmsg(db_);
            sqlite3_free(error);
            throw std::runtime_error("raw SQLite: " + message);
        }
    }

    std::string scalar(const char* sql) {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
        const int result = sqlite3_step(statement);
        if (result != SQLITE_ROW) {
            sqlite3_finalize(statement);
            throw std::runtime_error("raw SQLite scalar query returned no row");
        }
        const auto* value = sqlite3_column_text(statement, 0);
        const std::string answer = value != nullptr
                                       ? reinterpret_cast<const char*>(value)
                                       : "";
        sqlite3_finalize(statement);
        return answer;
    }

private:
    sqlite3* db_ = nullptr;
};

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
        require(thread->body == "Opening post", "opening post should be preserved");
        require(!thread->summary.created_at.empty(), "thread timestamp should be populated");
        require(thread->summary.reply_count == 0, "a new thread should have no replies");
        require(thread->summary.last_reply_at.empty(), "a new thread should have no reply timestamp");
        require(thread->replies.empty(), "a new thread should have an empty reply list");
        const auto initial_summaries = store.list_threads();
        require(initial_summaries.size() == 1 && initial_summaries.front().last_reply_at.empty(),
                "listing a thread without replies should have no reply timestamp");
        require(initial_summaries.front().id == thread->summary.id &&
                    initial_summaries.front().title == thread->summary.title &&
                    initial_summaries.front().created_at == thread->summary.created_at &&
                    initial_summaries.front().reply_count == thread->summary.reply_count,
                "thread listing should agree with its detail summary");

        reply_id = store.reply(thread_id, "First reply");
        require(reply_id > 0, "created replies should have positive IDs");
        thread = store.get_thread(thread_id);
        require(thread->summary.reply_count == 1, "reply count should update");
        require(thread->replies.size() == 1, "the reply should be returned");
        require(thread->replies.front().id == reply_id, "reply ID should be preserved");
        require(thread->replies.front().thread_id == thread_id, "reply should belong to its thread");
        require(thread->replies.front().body == "First reply", "reply body should be preserved");
        require(!thread->replies.front().created_at.empty(), "reply timestamp should be populated");
        require(thread->summary.last_reply_at == thread->replies.front().created_at,
                "thread detail should include its latest reply timestamp");
        require(store.list_threads().front().last_reply_at == thread->replies.front().created_at,
                "thread listing should include its latest reply timestamp");
    }

    {
        sshforum::Store reopened(database.path().string());
        auto thread = reopened.get_thread(thread_id);
        require(thread.has_value(), "thread should survive reopening the database");
        require(thread->summary.title == "First topic", "reopened thread title should match");
        require(thread->body == "Opening post", "reopened opening post should match");
        require(thread->summary.reply_count == 1, "reopened reply count should match");
        require(thread->replies.size() == 1 && thread->replies.front().id == reply_id,
                "reply should survive reopening the database");
        require(thread->summary.last_reply_at == thread->replies.front().created_at,
                "latest reply timestamp should survive reopening the database");
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
    require(first->summary.title == title && first->body == body,
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

void test_latest_reply_timestamp_uses_reply_id() {
    TemporaryDatabase database;
    sshforum::Store store(database.path().string());
    const auto thread_id = store.create_thread("Timestamp order", "Opening post");
    store.reply(thread_id, "First reply");
    store.reply(thread_id, "Second reply");

    {
        RawDatabase raw(database.path());
        raw.execute("UPDATE posts SET created_at = '2030-01-01T00:00:00.000Z' "
                    "WHERE body = 'First reply'");
        raw.execute("UPDATE posts SET created_at = '2020-01-01T00:00:00.000Z' "
                    "WHERE body = 'Second reply'");
    }

    const std::string latest_timestamp = "2020-01-01T00:00:00.000Z";
    const auto detail = store.get_thread(thread_id);
    require(detail && detail->replies.size() == 2 &&
                detail->summary.last_reply_at == detail->replies.back().created_at &&
                detail->summary.last_reply_at == latest_timestamp,
            "thread detail should use the timestamp of the highest reply ID");
    const auto summaries = store.list_threads();
    require(summaries.size() == 1 && summaries.front().last_reply_at == latest_timestamp,
            "thread listing should use the timestamp of the highest reply ID");
}

void test_legacy_migration_and_identity_secret() {
    TemporaryDatabase database;
    {
        RawDatabase raw(database.path());
        raw.execute("CREATE TABLE activity_clock ("
                    "id INTEGER PRIMARY KEY CHECK (id = 1), sequence INTEGER NOT NULL)");
        raw.execute("INSERT INTO activity_clock (id, sequence) VALUES (1, 30)");
        raw.execute("CREATE TABLE threads ("
                    "id INTEGER PRIMARY KEY, title TEXT NOT NULL, body TEXT NOT NULL, "
                    "created_at TEXT NOT NULL DEFAULT "
                    "(strftime('%Y-%m-%dT%H:%M:%fZ', 'now')), "
                    "activity_sequence INTEGER NOT NULL UNIQUE, "
                    "reply_count INTEGER NOT NULL DEFAULT 0)");
        raw.execute("CREATE TABLE posts ("
                    "id INTEGER PRIMARY KEY, "
                    "thread_id INTEGER NOT NULL REFERENCES threads(id) ON DELETE CASCADE, "
                    "body TEXT NOT NULL, created_at TEXT NOT NULL DEFAULT "
                    "(strftime('%Y-%m-%dT%H:%M:%fZ', 'now')))");
        raw.execute("CREATE INDEX posts_thread_id_id ON posts(thread_id, id)");
        raw.execute("INSERT INTO threads "
                    "(id, title, body, created_at, activity_sequence, reply_count) "
                    "VALUES (7, 'Old first', 'First body', '2020-01-01T00:00:00.000Z', 10, 1), "
                    "(9, 'Old second', 'Second body', '2020-02-01T00:00:00.000Z', 30, 0)");
        raw.execute("INSERT INTO posts (id, thread_id, body, created_at) VALUES "
                    "(21, 7, 'Old reply', '2020-01-02T00:00:00.000Z')");
        require(raw.scalar("PRAGMA user_version") == "0", "fixture must be a v0 database");
    }

    const std::string author = "v1:" + std::string(64, 'a');
    std::string first_candidate(32, 'A');
    first_candidate[0] = '\0';
    std::int64_t new_thread_id;
    {
        sshforum::Store store(database.path().string());
        const auto summaries = store.list_threads();
        require(summaries.size() == 2 && summaries[0].id == 9 && summaries[1].id == 7,
                "migration must preserve activity order and thread IDs");
        require(summaries[0].created_at == "2020-02-01T00:00:00.000Z" &&
                    summaries[1].created_at == "2020-01-01T00:00:00.000Z",
                "migration must preserve timestamps");
        require(summaries[0].author_id.empty() && summaries[1].author_id.empty(),
                "legacy threads must remain anonymous");
        const auto old = store.get_thread(7);
        require(old && old->summary.title == "Old first" &&
                    old->body == "First body" && old->summary.reply_count == 1 &&
                    old->replies.size() == 1 && old->replies[0].id == 21 &&
                    old->replies[0].body == "Old reply" &&
                    old->replies[0].created_at == "2020-01-02T00:00:00.000Z" &&
                    old->replies[0].author_id.empty(),
                "migration must preserve legacy post data and anonymity");

        require(store.get_or_create_identity_secret(first_candidate) == first_candidate,
                "the first valid candidate must become the secret, including binary bytes");
        require(store.get_or_create_identity_secret(std::string(32, 'B')) == first_candidate,
                "later candidates must return the stored secret");
        require_throws<std::invalid_argument>(
            [&] { store.get_or_create_identity_secret(std::string(31, 'x')); },
            "short secret candidates must be rejected");
        require_throws<std::invalid_argument>(
            [&] { store.get_or_create_identity_secret(std::string(33, 'x')); },
            "long secret candidates must be rejected");

        new_thread_id = store.create_thread("Named", "New body", author);
        const auto reply_id = store.reply(7, "Named reply", author);
        require(store.get_thread(new_thread_id)->summary.author_id == author,
                "new thread author ID must persist");
        const auto updated = store.get_thread(7);
        require(updated->replies.size() == 2 && updated->replies[1].id == reply_id &&
                    updated->replies[1].author_id == author,
                "new reply author ID must persist");
        require(store.list_threads()[0].id == 7,
                "a migrated thread must still rise on reply");

        require_throws<std::invalid_argument>(
            [&] { store.create_thread("Bad", "Body", "v1:" + std::string(63, 'a')); },
            "short author hashes must be rejected");
        require_throws<std::invalid_argument>(
            [&] { store.create_thread("Bad", "Body", "v1:" + std::string(64, 'A')); },
            "uppercase author hashes must be rejected");
        require_throws<std::invalid_argument>(
            [&] { store.reply(7, "Bad", "v2:" + std::string(64, 'a')); },
            "unknown author hash versions must be rejected");
    }
    {
        RawDatabase raw(database.path());
        require(raw.scalar("PRAGMA user_version") == "1", "migration must set v1");
        require(raw.scalar("SELECT sequence FROM activity_clock WHERE id = 1") == "32",
                "migration must retain the activity clock");
        require(raw.scalar("SELECT typeof(value) FROM forum_metadata "
                           "WHERE key = 'identity_hmac_key_v1'") == "blob",
                "the secret must be stored as a BLOB");
        raw.execute("INSERT INTO threads "
                    "(id, title, body, activity_sequence) "
                    "VALUES (15, 'Old SQL', 'Compatible', 33)");
        raw.execute("INSERT INTO posts (id, thread_id, body) "
                    "VALUES (100, 15, 'Old SQL reply')");
        require(raw.scalar("SELECT author_id FROM threads WHERE id = 15").empty() &&
                    raw.scalar("SELECT author_id FROM posts WHERE id = 100").empty(),
                "old explicit-column inserts must receive anonymous defaults");
    }
    {
        sshforum::Store reopened(database.path().string());
        require(reopened.get_or_create_identity_secret(std::string(32, 'C')) == first_candidate,
                "reopening must retain the original secret");
        require(reopened.get_thread(new_thread_id)->summary.author_id == author,
                "reopening must retain author IDs");
        const auto old_sql_thread = reopened.get_thread(15);
        require(old_sql_thread && old_sql_thread->summary.author_id.empty() &&
                    old_sql_thread->replies.size() == 1 &&
                    old_sql_thread->replies[0].author_id.empty(),
                "old SQL inserts must be readable as anonymous");
    }
    {
        RawDatabase raw(database.path());
        raw.execute("UPDATE forum_metadata SET value = x'00' "
                    "WHERE key = 'identity_hmac_key_v1'");
    }
    {
        sshforum::Store reopened(database.path().string());
        require_throws<std::runtime_error>(
            [&] { reopened.get_or_create_identity_secret(first_candidate); },
            "a malformed stored secret must fail without replacement");
    }
    {
        RawDatabase raw(database.path());
        require(raw.scalar("SELECT length(value) FROM forum_metadata "
                           "WHERE key = 'identity_hmac_key_v1'") == "1",
                "a malformed stored secret must not be reset");
    }
}

void test_future_version_is_untouched() {
    TemporaryDatabase database;
    {
        RawDatabase raw(database.path());
        raw.execute("PRAGMA user_version = 2");
    }
    require_throws<std::runtime_error>(
        [&] { sshforum::Store store(database.path().string()); },
        "a future database version must be rejected");
    {
        RawDatabase raw(database.path());
        require(raw.scalar("PRAGMA user_version") == "2",
                "a future version must remain unchanged");
        require(raw.scalar("SELECT count(*) FROM sqlite_master WHERE type = 'table'") == "0",
                "future databases must not gain tables");
        require(raw.scalar("PRAGMA journal_mode") == "delete",
                "future databases must not be switched to WAL");
    }
}

}  // namespace

int main() {
    try {
        test_crud_and_persistence();
        test_validation_and_missing_thread();
        test_sql_metacharacters();
        test_latest_reply_sorting_and_limit();
        test_latest_reply_timestamp_uses_reply_id();
        test_legacy_migration_and_identity_secret();
        test_future_version_is_untouched();
        std::cout << "store tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "store tests failed: " << error.what() << '\n';
        return 1;
    }
}
