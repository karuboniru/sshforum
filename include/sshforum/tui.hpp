#pragma once

#include "sshforum/store.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sshforum {

class Tui {
public:
    struct InputResult {
        std::string output;
        std::size_t consumed;
    };

    explicit Tui(Store& store);

    void set_author_id(std::string author_id);
    void resize(int width, int height);
    std::string start();
    std::string input(std::string_view bytes);
    // Resume with bytes.substr(consumed); individual commands and rendering finish atomically.
    InputResult input_some(std::string_view bytes);
    bool done() const noexcept { return done_; }
    static std::string restore_terminal();

private:
    enum class Page { list, thread, new_title, new_body, reply_body };
    enum class Escape { none, esc, csi, ss3 };

    Store& store_;
    std::string author_id_;
    Page page_ = Page::list;
    Escape escape_ = Escape::none;
    std::chrono::steady_clock::time_point escape_at_{};
    std::string escape_data_;
    std::string utf8_pending_;
    int utf8_expected_ = 0;
    bool last_was_cr_ = false;
    int width_ = 80;
    int height_ = 24;
    bool started_ = false;
    bool done_ = false;
    bool dirty_ = true;
    std::size_t dispatched_keys_ = 0;
    std::vector<ThreadSummary> threads_;
    int selected_ = 0;
    int list_top_ = 0;
    int thread_scroll_ = 0;
    std::int64_t thread_id_ = 0;
    Thread thread_{};
    std::string draft_title_;
    std::string draft_body_;
    std::size_t editor_cursor_ = 0;
    int editor_top_ = 0;
    int preferred_column_ = -1;
    bool editor_wrap_end_ = false;
    std::string status_;

    void refresh_list();
    void refresh_thread();
    void key(std::string_view name);
    void character(std::string_view utf8);
    void byte(unsigned char value);
    InputResult input_impl(std::string_view bytes, bool bounded);
    void submit();
    void cancel_editor();
    void show_thread(std::int64_t id);
    std::string render();
};

} // namespace sshforum
