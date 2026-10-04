#pragma once

#include <coroutine>
#include <exception>
#include <utility>

namespace sshforum {
// A cooperative session: every suspension returns control to the poll loop.
// The owner must outlive the coroutine and must only resume it on that loop.
class Task {
public:
    struct promise_type {
        std::exception_ptr error;
        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { error = std::current_exception(); }
    };
    Task() = default;
    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    ~Task() { if (handle_) handle_.destroy(); }
    bool done() const { return !handle_ || handle_.done(); }
    void resume() {
        if (!done()) handle_.resume();
        if (handle_ && handle_.promise().error)
            std::rethrow_exception(handle_.promise().error);
    }
private:
    explicit Task(std::coroutine_handle<promise_type> handle) : handle_(handle) {}
    std::coroutine_handle<promise_type> handle_{};
};
} // namespace sshforum
