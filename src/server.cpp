#include "sshforum/server.hpp"
#include "sshforum/store.hpp"
#include "sshforum/task.hpp"
#include "sshforum/tui.hpp"

#include <libssh/callbacks.h>
#include <libssh/libssh.h>
#include <libssh/server.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <type_traits>
#include <vector>

namespace sshforum {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
volatile std::sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }

template <typename T, auto Free>
using Handle = std::unique_ptr<std::remove_pointer_t<T>, decltype(Free)>;

void ensure_parent(const std::string& path) {
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
}

void ensure_host_key(const std::string& path) {
    if (std::filesystem::exists(path)) return;
    ensure_parent(path);
    ssh_key raw = nullptr;
#if LIBSSH_VERSION_INT >= SSH_VERSION_INT(0, 12, 0)
    if (ssh_pki_generate_key(SSH_KEYTYPE_ED25519, nullptr, &raw) != SSH_OK)
#else
    if (ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &raw) != SSH_OK)
#endif
        throw std::runtime_error("cannot generate Ed25519 host key");
    Handle<ssh_key, ssh_key_free> key(raw, ssh_key_free);
    if (ssh_pki_export_privkey_file(key.get(), nullptr, nullptr, nullptr, path.c_str()) != SSH_OK)
        throw std::runtime_error("cannot save host key: " + path);
}

class Connection {
public:
    explicit Connection(Store& store)
        : session_(ssh_new(), ssh_free), event_(ssh_event_new(), ssh_event_free), tui_(store) {
        if (!session_ || !event_) throw std::runtime_error("cannot allocate SSH session");
    }
    ~Connection() {
        task_ = {}; // Destroy the coroutine before any state it references.
        if (registered_) ssh_event_remove_session(event_.get(), session_.get());
        if (channel_) {
            ssh_channel_close(channel_);
            ssh_channel_free(channel_);
        }
        ssh_disconnect(session_.get());
    }
    ssh_session session() const { return session_.get(); }
    void start() {
        ssh_set_blocking(session(), 0);
        ssh_callbacks_init(&server_callbacks_);
        server_callbacks_.userdata = this;
        server_callbacks_.auth_none_function = [](ssh_session, const char*, void* data) {
            return authenticate(data);
        };
        server_callbacks_.auth_password_function = [](ssh_session, const char*, const char*, void* data) {
            return authenticate(data);
        };
        server_callbacks_.auth_pubkey_function = [](ssh_session, const char*, ssh_key, char state, void* data) -> int {
            // Every key is allowed. An unsigned key probe is not a completed login.
            if (state == SSH_PUBLICKEY_STATE_VALID) authenticate(data);
            return SSH_AUTH_SUCCESS;
        };
        server_callbacks_.channel_open_request_session_function = [](ssh_session session, void* data) {
            auto& self = *static_cast<Connection*>(data);
            if (!self.authenticated_ || self.channel_) return static_cast<ssh_channel>(nullptr);
            self.channel_ = ssh_channel_new(session);
            if (self.channel_) ssh_set_channel_callbacks(self.channel_, &self.channel_callbacks_);
            return self.channel_;
        };
        ssh_callbacks_init(&channel_callbacks_);
        channel_callbacks_.userdata = this;
        channel_callbacks_.channel_pty_request_function = [](ssh_session, ssh_channel, const char*,
                                                               int width, int height, int, int, void* data) {
            auto& self = *static_cast<Connection*>(data);
            self.width_ = width;
            self.height_ = height;
            self.resized_ = true;
            return 0;
        };
        channel_callbacks_.channel_pty_window_change_function = [](ssh_session, ssh_channel,
                                                                     int width, int height, int, int, void* data) {
            auto& self = *static_cast<Connection*>(data);
            self.width_ = width;
            self.height_ = height;
            self.resized_ = true;
            return 0;
        };
        channel_callbacks_.channel_shell_request_function = [](ssh_session, ssh_channel, void* data) {
            auto& self = *static_cast<Connection*>(data);
            if (self.shell_) return 1;
            self.shell_ = true;
            return 0;
        };
        if (ssh_set_server_callbacks(session(), &server_callbacks_) != SSH_OK)
            throw std::runtime_error("cannot set SSH callbacks");
        // Also accept keyboard-interactive and other authentication messages
        // without consulting usernames, secrets, or system accounts.
        ssh_set_message_callback(session(), [](ssh_session, ssh_message message, void* data) {
            if (ssh_message_type(message) == SSH_REQUEST_AUTH) {
                authenticate(data);
                ssh_message_auth_reply_success(message, 0);
                return 0;
            }
            return 1; // libssh rejects unsupported exec, subsystem and forwarding requests.
        }, this);
        ssh_set_auth_methods(session(), SSH_AUTH_METHOD_NONE | SSH_AUTH_METHOD_PASSWORD |
                                       SSH_AUTH_METHOD_PUBLICKEY | SSH_AUTH_METHOD_INTERACTIVE);
        task_ = run();
    }
    bool tick() {
        try {
            task_.resume();
            return !task_.done();
        } catch (const std::exception& error) {
            std::cerr << "session: " << error.what() << '\n';
            return false;
        }
    }
    pollfd descriptor() const {
        short events = POLLIN;
        if (ssh_get_poll_flags(session()) & SSH_WRITE_PENDING) events |= POLLOUT;
        return {ssh_get_fd(session()), events, 0};
    }
private:
    static int authenticate(void* data) {
        static_cast<Connection*>(data)->authenticated_ = true;
        return SSH_AUTH_SUCCESS;
    }
    bool pump() {
        return ssh_event_dopoll(event_.get(), 0) != SSH_ERROR && ssh_is_connected(session());
    }
    void enqueue(std::string output) {
        if (output.empty()) return;
        if (pending_.empty()) last_write_ = Clock::now();
        pending_ += output;
        if (pending_.size() > 4 * 1024 * 1024)
            throw std::runtime_error("client output queue exceeded");
    }
    bool flush() {
        if (pending_.empty()) return true;
        const int written = ssh_channel_write(channel_, pending_.data(),
            static_cast<uint32_t>(std::min<std::size_t>(pending_.size(), 32768)));
        if (written == SSH_ERROR) return false;
        if (written > 0) {
            pending_.erase(0, static_cast<std::size_t>(written));
            last_write_ = Clock::now();
        }
        return Clock::now() - last_write_ < 30s;
    }
    Task run() {
        const auto deadline = Clock::now() + 30s;
        int result;
        while ((result = ssh_handle_key_exchange(session())) == SSH_AGAIN) {
            if (stopping || Clock::now() >= deadline) co_return;
            co_await std::suspend_always{};
        }
        if (result != SSH_OK) co_return;
        if (ssh_event_add_session(event_.get(), session()) != SSH_OK) co_return;
        registered_ = true;
        while (!shell_) {
            if (stopping || Clock::now() >= deadline || !pump()) co_return;
            co_await std::suspend_always{};
        }
        tui_.resize(width_, height_);
        resized_ = false;
        enqueue(tui_.start());
        auto last_input = Clock::now();
        std::array<char, 4096> buffer{};
        while (!tui_.done() && !stopping) {
            if (!pump() || !ssh_channel_is_open(channel_) || ssh_channel_is_eof(channel_)) co_return;
            if (resized_) {
                tui_.resize(width_, height_);
                resized_ = false;
            }
            // Bound work and buffered output per tick so a busy peer cannot
            // monopolize the loop. SQLite operations are deliberately synchronous.
            if (pending_.size() < 256 * 1024) {
                const int count = ssh_channel_read_nonblocking(channel_, buffer.data(), buffer.size(), 0);
                if (count == SSH_ERROR) co_return;
                if (count > 0) {
                    last_input = Clock::now();
                    enqueue(tui_.input(std::string_view(buffer.data(), static_cast<std::size_t>(count))));
                } else {
                    enqueue(tui_.input(""));
                }
            }
            if (!flush() || Clock::now() - last_input > 30min) co_return;
            co_await std::suspend_always{};
        }
        // Normal user exit already returns this sequence from Tui::input().
        if (!tui_.done()) enqueue(Tui::restore_terminal());
        const auto close_deadline = Clock::now() + 2s;
        while ((!pending_.empty() || (ssh_get_poll_flags(session()) & SSH_WRITE_PENDING)) &&
               Clock::now() < close_deadline) {
            if (!pump() || !flush()) co_return;
            co_await std::suspend_always{};
        }
        const auto final_deadline = Clock::now() + 2s;
        for (int stage = 0; stage < 3;) {
            const int status = stage == 0 ? ssh_channel_request_send_exit_status(channel_, 0) :
                               stage == 1 ? ssh_channel_send_eof(channel_) : ssh_channel_close(channel_);
            if (status == SSH_ERROR || Clock::now() >= final_deadline) co_return;
            if (status == SSH_OK) ++stage;
            else {
                if (!pump()) co_return;
                co_await std::suspend_always{};
            }
        }
        // These control packets were queued after the screen-output drain.
        // Keep pumping until they reach the socket, or the peer times out.
        while ((ssh_get_poll_flags(session()) & SSH_WRITE_PENDING) && Clock::now() < final_deadline) {
            if (!pump()) co_return;
            co_await std::suspend_always{};
        }
    }

    Handle<ssh_session, ssh_free> session_;
    Handle<ssh_event, ssh_event_free> event_;
    ssh_channel channel_ = nullptr;
    ssh_server_callbacks_struct server_callbacks_{};
    ssh_channel_callbacks_struct channel_callbacks_{};
    bool authenticated_ = false, shell_ = false, resized_ = false, registered_ = false;
    int width_ = 80, height_ = 24;
    Tui tui_;
    std::string pending_;
    Clock::time_point last_write_{};
    Task task_;
};
} // namespace

int run_server(const ServerOptions& options) {
    // Newly created database and private host-key files belong only to this user.
    umask(0077);
    stopping = 0;
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    std::signal(SIGPIPE, SIG_IGN);
    ensure_parent(options.database);
    Store store(options.database);
    ensure_host_key(options.host_key);
    Handle<ssh_bind, ssh_bind_free> listener(ssh_bind_new(), ssh_bind_free);
    if (!listener) throw std::runtime_error("cannot allocate SSH listener");
    auto set = [&](ssh_bind_options_e option, const void* value) {
        if (ssh_bind_options_set(listener.get(), option, value) != SSH_OK)
            throw std::runtime_error(ssh_get_error(listener.get()));
    };
    const bool process_config = false;
    set(SSH_BIND_OPTIONS_PROCESS_CONFIG, &process_config);
    set(SSH_BIND_OPTIONS_BINDADDR, options.bind.c_str());
    set(SSH_BIND_OPTIONS_BINDPORT, &options.port);
    set(SSH_BIND_OPTIONS_HOSTKEY, options.host_key.c_str());
    if (ssh_bind_listen(listener.get()) != SSH_OK)
        throw std::runtime_error(ssh_get_error(listener.get()));
    ssh_bind_set_blocking(listener.get(), 0);
    std::cout << "sshforum listening on " << options.bind << ':' << options.port
              << " | anonymous access | SQLite: " << options.database << std::endl;

    std::vector<std::unique_ptr<Connection>> connections;
    while (!stopping) {
        std::vector<pollfd> descriptors;
        descriptors.reserve(connections.size() + 1);
        descriptors.push_back({ssh_bind_get_fd(listener.get()), POLLIN, 0});
        for (const auto& connection : connections) descriptors.push_back(connection->descriptor());
        // A bounded timer also services escape-key disambiguation and deadlines.
        const int ready = poll(descriptors.data(), descriptors.size(), 50);
        if (ready < 0 && errno != EINTR) throw std::runtime_error("poll failed");
        if (stopping) break;
        if (ready > 0 && (descriptors.front().revents & POLLIN)) {
            auto connection = std::make_unique<Connection>(store);
            if (ssh_bind_accept(listener.get(), connection->session()) == SSH_OK &&
                connections.size() < static_cast<std::size_t>(options.max_sessions)) {
                connection->start();
                connections.push_back(std::move(connection));
            }
        }
        std::erase_if(connections, [](auto& connection) { return !connection->tick(); });
    }
    // Give active terminals a chance to restore their screen on SIGINT/SIGTERM.
    const auto shutdown_deadline = Clock::now() + 2s;
    while (!connections.empty() && Clock::now() < shutdown_deadline) {
        std::erase_if(connections, [](auto& connection) { return !connection->tick(); });
        if (!connections.empty()) poll(nullptr, 0, 20);
    }
    std::cout << "sshforum stopped\n";
    return 0;
}
} // namespace sshforum
