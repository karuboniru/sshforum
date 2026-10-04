#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sshforum {

struct ThreadSummary {
    std::int64_t id;
    std::string title;
    std::string created_at;
    std::int64_t reply_count;
    std::string author_id{};
    std::string last_reply_at{};
};

struct Post {
    std::int64_t id;
    std::int64_t thread_id;
    std::string body;
    std::string created_at;
    std::string author_id{};
};

struct Thread {
    ThreadSummary summary;
    std::string body;
    std::vector<Post> replies;
};

class Store {
public:
    explicit Store(const std::string& path);
    ~Store();

    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    Store(Store&&) = delete;
    Store& operator=(Store&&) = delete;

    std::vector<ThreadSummary> list_threads(int limit = 200);
    std::optional<Thread> get_thread(std::int64_t id);
    std::int64_t create_thread(const std::string& title, const std::string& body,
                               const std::string& author_id = "");
    std::int64_t reply(std::int64_t thread_id, const std::string& body,
                       const std::string& author_id = "");
    std::string get_or_create_identity_secret(const std::string& candidate);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sshforum
