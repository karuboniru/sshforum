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

    void bind_blob(int index, const std::string& value) {
        if (sqlite3_bind_blob(statement_, index, value.data(),
                              static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
            database_error(db_, "bind blob");
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

    std::string blob(int column) const {
        if (sqlite3_column_type(statement_, column) != SQLITE_BLOB) {
            throw std::runtime_error("identity secret is not a BLOB");
        }
        const auto* value = sqlite3_column_blob(statement_, column);
        const int size = sqlite3_column_bytes(statement_, column);
        if (size == 0) {
            return {};
        }
        return std::string(static_cast<const char*>(value), static_cast<std::size_t>(size));
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

void validate_author_id(const std::string& author_id) {
    if (author_id.empty()) {
        return;
    }
    const bool valid = author_id.size() == 67 && author_id.compare(0, 3, "v1:") == 0 &&
                       std::all_of(author_id.begin() + 3, author_id.end(), [](char character) {
                           return (character >= '0' && character <= '9') ||
                                  (character >= 'a' && character <= 'f');
                       });
    if (!valid) {
        throw std::invalid_argument("author_id must be empty or v1: followed by 64 lowercase hex digits");
    }
}

std::int64_t user_version(sqlite3* db) {
    Statement statement(db, "PRAGMA user_version");
    if (statement.step() != SQLITE_ROW) {
        throw std::runtime_error("SQLite user_version is missing");
    }
    return statement.integer(0);
}

bool has_column(sqlite3* db, const char* table_info_sql, std::string_view name) {
    Statement statement(db, table_info_sql);
    while (statement.step() == SQLITE_ROW) {
        if (statement.text(1) == name) {
            return true;
        }
    }
    return false;
}

ThreadSummary read_summary(const Statement& statement) {
    return {statement.integer(0), statement.text(1), statement.text(2),
            statement.text(3), statement.integer(4), statement.text(5), statement.text(6)};
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
            if (user_version(db) > 1) {
                throw std::runtime_error("SQLite database version is newer than this application supports");
            }
            execute(db, "PRAGMA foreign_keys = ON");
            if (user_version(db) == 0) {
                Transaction transaction(db, "BEGIN IMMEDIATE");
                const auto version = user_version(db);
                if (version > 1) {
                    throw std::runtime_error("SQLite database version is newer than this application supports");
                }
                if (version == 0) {
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
                    if (!has_column(db, "PRAGMA table_info(threads)", "author_id")) {
                        execute(db, "ALTER TABLE threads ADD COLUMN author_id TEXT NOT NULL DEFAULT ''");
                    }
                    if (!has_column(db, "PRAGMA table_info(posts)", "author_id")) {
                        execute(db, "ALTER TABLE posts ADD COLUMN author_id TEXT NOT NULL DEFAULT ''");
                    }
                    execute(db, "CREATE INDEX IF NOT EXISTS posts_thread_id_id ON posts(thread_id, id)");
                    execute(db,
                            "CREATE TABLE IF NOT EXISTS forum_metadata ("
                            "  key TEXT PRIMARY KEY,"
                            "  value BLOB NOT NULL"
                            ")");
                    execute(db, "PRAGMA user_version = 1");
                }
                transaction.commit();
            }
            execute(db, "PRAGMA journal_mode = WAL");
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
                        "SELECT id, title, body, created_at, reply_count, author_id, "
                        "COALESCE((SELECT created_at FROM posts "
                        "WHERE thread_id = threads.id ORDER BY id DESC LIMIT 1), '') "
                        "FROM threads "
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
                            "SELECT id, title, body, created_at, reply_count, author_id, "
                            "COALESCE((SELECT created_at FROM posts "
                            "WHERE thread_id = threads.id ORDER BY id DESC LIMIT 1), '') "
                            "FROM threads WHERE id = ?1");
        statement.bind(1, id);
        if (statement.step() == SQLITE_ROW) {
            thread.emplace(Thread{read_summary(statement), {}});
        }
    }
    if (thread) {
        Statement statement(impl_->db,
                            "SELECT id, thread_id, body, created_at, author_id FROM posts "
                            "WHERE thread_id = ?1 ORDER BY id ASC");
        statement.bind(1, id);
        while (statement.step() == SQLITE_ROW) {
            thread->replies.push_back({statement.integer(0), statement.integer(1),
                                       statement.text(2), statement.text(3), statement.text(4)});
        }
    }
    transaction.commit();
    return thread;
}

std::int64_t Store::create_thread(const std::string& title, const std::string& body,
                                  const std::string& author_id) {
    validate_text(title, 120, "title");
    validate_text(body, 16384, "body");
    validate_author_id(author_id);

    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->db, "BEGIN IMMEDIATE");
    const std::int64_t activity = next_activity(impl_->db);
    Statement statement(impl_->db,
                        "INSERT INTO threads (title, body, activity_sequence, author_id) "
                        "VALUES (?1, ?2, ?3, ?4)");
    statement.bind(1, title);
    statement.bind(2, body);
    statement.bind(3, activity);
    statement.bind(4, author_id);
    statement.step();
    const std::int64_t id = sqlite3_last_insert_rowid(impl_->db);
    transaction.commit();
    return id;
}

std::int64_t Store::reply(std::int64_t thread_id, const std::string& body,
                          const std::string& author_id) {
    validate_text(body, 16384, "body");
    validate_author_id(author_id);

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
        Statement statement(impl_->db,
                            "INSERT INTO posts (thread_id, body, author_id) VALUES (?1, ?2, ?3)");
        statement.bind(1, thread_id);
        statement.bind(2, body);
        statement.bind(3, author_id);
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

std::string Store::get_or_create_identity_secret(const std::string& candidate) {
    if (candidate.size() != 32) {
        throw std::invalid_argument("identity secret candidate must be exactly 32 bytes");
    }

    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->db, "BEGIN IMMEDIATE");
    {
        Statement statement(impl_->db,
                            "INSERT INTO forum_metadata (key, value) "
                            "VALUES ('identity_hmac_key_v1', ?1) "
                            "ON CONFLICT(key) DO NOTHING");
        statement.bind_blob(1, candidate);
        statement.step();
    }
    Statement statement(impl_->db,
                        "SELECT value FROM forum_metadata WHERE key = 'identity_hmac_key_v1'");
    if (statement.step() != SQLITE_ROW) {
        throw std::runtime_error("identity secret is missing");
    }
    const std::string secret = statement.blob(0);
    if (secret.size() != 32) {
        throw std::runtime_error("stored identity secret must be exactly 32 bytes");
    }
    transaction.commit();
    return secret;
}

}  // namespace sshforum
