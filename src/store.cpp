#include "sshforum/store.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>

namespace sshforum {
namespace {

[[noreturn]] void database_error(sqlite3* db, std::string_view operation) {
    throw std::runtime_error(std::string(operation) + ": " + sqlite3_errmsg(db));
}

void execute(sqlite3* db, const char* sql) {
    char* error = nullptr;
    const int result = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if (result != SQLITE_OK) {
        const std::string message = error != nullptr ? error : sqlite3_errmsg(db);
        sqlite3_free(error);
        throw std::runtime_error("SQLite: " + message);
    }
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr) != SQLITE_OK) {
            database_error(db, "prepare statement");
        }
    }

    ~Statement() { sqlite3_finalize(statement_); }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void bind(int index, std::int64_t value) {
        if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) {
            database_error(db_, "bind integer");
        }
    }

    void bind(int index, const std::string& value) {
        if (sqlite3_bind_text(statement_, index, value.data(),
                              static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
            database_error(db_, "bind text");
        }
    }

    int step() {
        const int result = sqlite3_step(statement_);
        if (result != SQLITE_ROW && result != SQLITE_DONE) {
            database_error(db_, "execute statement");
        }
        return result;
    }

    std::int64_t integer(int column) const {
        return sqlite3_column_int64(statement_, column);
    }

    std::string text(int column) const {
        const auto* value = sqlite3_column_text(statement_, column);
        const int size = sqlite3_column_bytes(statement_, column);
        return value != nullptr
                   ? std::string(reinterpret_cast<const char*>(value),
                                 static_cast<std::size_t>(size))
                   : std::string{};
    }

private:
    sqlite3* db_;
    sqlite3_stmt* statement_ = nullptr;
};

class Transaction {
public:
    Transaction(sqlite3* db, const char* begin) : db_(db) { execute(db_, begin); }

    ~Transaction() {
        if (active_) {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        }
    }

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    void commit() {
        execute(db_, "COMMIT");
        active_ = false;
    }

private:
    sqlite3* db_;
    bool active_ = true;
};

void validate_text(const std::string& value, std::size_t max_bytes,
                   std::string_view field) {
    if (value.empty() || value.size() > max_bytes ||
        std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return std::isspace(character) != 0;
        })) {
        throw std::invalid_argument(std::string(field) + " must contain 1 to " +
                                    std::to_string(max_bytes) +
                                    " bytes of non-whitespace text");
    }
}

ThreadSummary read_summary(const Statement& statement) {
    return {statement.integer(0), statement.text(1), statement.text(2),
            statement.text(3), statement.integer(4)};
}

std::int64_t next_activity(sqlite3* db) {
    execute(db, "UPDATE activity_clock SET sequence = sequence + 1 WHERE id = 1");
    Statement statement(db, "SELECT sequence FROM activity_clock WHERE id = 1");
    if (statement.step() != SQLITE_ROW) {
        throw std::runtime_error("SQLite activity clock is missing");
    }
    return statement.integer(0);
}

}  // namespace

struct Store::Impl {
    explicit Impl(const std::string& path) {
        const int result = sqlite3_open_v2(
            path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
            nullptr);
        if (result != SQLITE_OK) {
            const std::string message = db != nullptr ? sqlite3_errmsg(db) : "unknown error";
            sqlite3_close(db);
            db = nullptr;
            throw std::runtime_error("open SQLite database: " + message);
        }

        try {
            if (sqlite3_busy_timeout(db, 5000) != SQLITE_OK) {
                database_error(db, "set SQLite busy timeout");
            }
            execute(db, "PRAGMA journal_mode = WAL");
            execute(db, "PRAGMA foreign_keys = ON");
            execute(db,
                    "CREATE TABLE IF NOT EXISTS activity_clock ("
                    "  id INTEGER PRIMARY KEY CHECK (id = 1),"
                    "  sequence INTEGER NOT NULL"
                    ")");
            execute(db, "INSERT OR IGNORE INTO activity_clock (id, sequence) VALUES (1, 0)");
            execute(db,
                    "CREATE TABLE IF NOT EXISTS threads ("
                    "  id INTEGER PRIMARY KEY,"
                    "  title TEXT NOT NULL,"
                    "  body TEXT NOT NULL,"
                    "  created_at TEXT NOT NULL DEFAULT "
                    "    (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')) ,"
                    "  activity_sequence INTEGER NOT NULL UNIQUE,"
                    "  reply_count INTEGER NOT NULL DEFAULT 0"
                    ")");
            execute(db,
                    "CREATE TABLE IF NOT EXISTS posts ("
                    "  id INTEGER PRIMARY KEY,"
                    "  thread_id INTEGER NOT NULL REFERENCES threads(id) ON DELETE CASCADE,"
                    "  body TEXT NOT NULL,"
                    "  created_at TEXT NOT NULL DEFAULT "
                    "    (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))"
                    ")");
            execute(db, "CREATE INDEX IF NOT EXISTS posts_thread_id_id ON posts(thread_id, id)");
        } catch (...) {
            sqlite3_close(db);
            db = nullptr;
            throw;
        }
    }

    ~Impl() { sqlite3_close(db); }

    sqlite3* db = nullptr;
    std::mutex mutex;
};

Store::Store(const std::string& path) : impl_(std::make_unique<Impl>(path)) {}
Store::~Store() = default;

std::vector<ThreadSummary> Store::list_threads(int limit) {
    if (limit <= 0) {
        throw std::invalid_argument("limit must be positive");
    }

    std::lock_guard lock(impl_->mutex);
    Statement statement(impl_->db,
                        "SELECT id, title, body, created_at, reply_count FROM threads "
                        "ORDER BY activity_sequence DESC LIMIT ?1");
    statement.bind(1, static_cast<std::int64_t>(limit));
    std::vector<ThreadSummary> threads;
    while (statement.step() == SQLITE_ROW) {
        threads.push_back(read_summary(statement));
    }
    return threads;
}

std::optional<Thread> Store::get_thread(std::int64_t id) {
    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->db, "BEGIN");
    std::optional<Thread> thread;
    {
        Statement statement(impl_->db,
                            "SELECT id, title, body, created_at, reply_count "
                            "FROM threads WHERE id = ?1");
        statement.bind(1, id);
        if (statement.step() == SQLITE_ROW) {
            thread.emplace(Thread{read_summary(statement), {}});
        }
    }
    if (thread) {
        Statement statement(impl_->db,
                            "SELECT id, thread_id, body, created_at FROM posts "
                            "WHERE thread_id = ?1 ORDER BY id ASC");
        statement.bind(1, id);
        while (statement.step() == SQLITE_ROW) {
            thread->replies.push_back({statement.integer(0), statement.integer(1),
                                       statement.text(2), statement.text(3)});
        }
    }
    transaction.commit();
    return thread;
}

std::int64_t Store::create_thread(const std::string& title, const std::string& body) {
    validate_text(title, 120, "title");
    validate_text(body, 16384, "body");

    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->db, "BEGIN IMMEDIATE");
    const std::int64_t activity = next_activity(impl_->db);
    Statement statement(impl_->db,
                        "INSERT INTO threads (title, body, activity_sequence) "
                        "VALUES (?1, ?2, ?3)");
    statement.bind(1, title);
    statement.bind(2, body);
    statement.bind(3, activity);
    statement.step();
    const std::int64_t id = sqlite3_last_insert_rowid(impl_->db);
    transaction.commit();
    return id;
}

std::int64_t Store::reply(std::int64_t thread_id, const std::string& body) {
    validate_text(body, 16384, "body");

    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->db, "BEGIN IMMEDIATE");
    {
        Statement statement(impl_->db, "SELECT id FROM threads WHERE id = ?1");
        statement.bind(1, thread_id);
        if (statement.step() == SQLITE_DONE) {
            throw std::out_of_range("thread does not exist");
        }
    }
    std::int64_t id;
    {
        Statement statement(impl_->db, "INSERT INTO posts (thread_id, body) VALUES (?1, ?2)");
        statement.bind(1, thread_id);
        statement.bind(2, body);
        statement.step();
        id = sqlite3_last_insert_rowid(impl_->db);
    }
    const std::int64_t activity = next_activity(impl_->db);
    {
        Statement statement(impl_->db,
                            "UPDATE threads SET reply_count = reply_count + 1, "
                            "activity_sequence = ?1 WHERE id = ?2");
        statement.bind(1, activity);
        statement.bind(2, thread_id);
        statement.step();
    }
    transaction.commit();
    return id;
}

}  // namespace sshforum
