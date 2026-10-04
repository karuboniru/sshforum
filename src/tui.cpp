#include "sshforum/tui.hpp"
#include "sshforum/identity.hpp"

#include <algorithm>
#include <cstddef>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sshforum {
namespace {

// Only validated printable Unicode reaches the screen. In particular, a value
// read from the database cannot inject an ANSI control sequence.
bool decode(std::string_view text, std::size_t& at, char32_t& cp, std::string_view& bytes) {
    if (at >= text.size()) return false;
    const std::size_t start = at;
    const auto first = static_cast<unsigned char>(text[at++]);
    int count = 0;
    if (first < 0x80) cp = first;
    else if (first >= 0xc2 && first <= 0xdf) { cp = first & 0x1f; count = 1; }
    else if (first >= 0xe0 && first <= 0xef) { cp = first & 0x0f; count = 2; }
    else if (first >= 0xf0 && first <= 0xf4) { cp = first & 0x07; count = 3; }
    else return false;
    if (at + static_cast<std::size_t>(count) > text.size()) return false;
    for (int i = 0; i < count; ++i) {
        const auto next = static_cast<unsigned char>(text[at]);
        if ((next & 0xc0) != 0x80) return false;
        cp = (cp << 6) | (next & 0x3f);
        ++at;
    }
    if ((count == 1 && cp < 0x80) || (count == 2 && cp < 0x800) ||
        (count == 3 && cp < 0x10000) || (cp >= 0xd800 && cp <= 0xdfff) ||
        cp > 0x10ffff) return false;
    bytes = text.substr(start, at - start);
    return true;
}

bool printable(char32_t cp) {
    return cp >= 0x20 && cp != 0x7f && !(cp >= 0x80 && cp <= 0x9f) &&
           cp != 0x2028 && cp != 0x2029;
}

int cells(char32_t cp) {
    if ((cp >= 0x300 && cp <= 0x36f) || (cp >= 0x1ab0 && cp <= 0x1aff) ||
        (cp >= 0x1dc0 && cp <= 0x1dff) || (cp >= 0x20d0 && cp <= 0x20ff) ||
        (cp >= 0xfe20 && cp <= 0xfe2f)) return 0;
    if ((cp >= 0x1100 && cp <= 0x115f) || (cp >= 0x2329 && cp <= 0x232a) ||
        (cp >= 0x2e80 && cp <= 0xa4cf) || (cp >= 0xac00 && cp <= 0xd7a3) ||
        (cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0xfe10 && cp <= 0xfe19) ||
        (cp >= 0xfe30 && cp <= 0xfe6f) || (cp >= 0xff00 && cp <= 0xff60) ||
        (cp >= 0xffe0 && cp <= 0xffe6) || (cp >= 0x1f300 && cp <= 0x1faff) ||
        (cp >= 0x20000 && cp <= 0x3fffd)) return 2;
    return 1;
}

std::string fit(std::string_view source, int width) {
    std::string out;
    int used = 0;
    for (std::size_t at = 0; at < source.size();) {
        char32_t cp = 0;
        std::string_view bytes;
        if (!decode(source, at, cp, bytes)) continue;
        if (cp == '\n' || cp == '\r' || cp == '\t') {
            if (used >= width) break;
            out.push_back(' ');
            ++used;
        } else if (printable(cp)) {
            const int size = cells(cp);
            if (used + size > width) break;
            out.append(bytes);
            used += size;
        }
    }
    return out;
}

int display_width(std::string_view text) {
    int width = 0;
    for (std::size_t at = 0; at < text.size();) {
        char32_t cp = 0;
        std::string_view bytes;
        if (decode(text, at, cp, bytes) && printable(cp)) width += cells(cp);
    }
    return width;
}

std::string aligned(std::string_view left, std::string_view right, int width) {
    const auto rhs = fit(right, width);
    const int right_width = display_width(rhs);
    const auto lhs = fit(left, std::max(0, width - right_width - 2));
    return lhs + std::string(std::max(0, width - display_width(lhs) - right_width), ' ') + rhs;
}

std::string relative_time(std::string_view timestamp, std::chrono::system_clock::time_point now) {
    using namespace std::chrono;
    int y, mo, d, h, mi, sec;
    const std::string text(timestamp);
    if (std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6)
        return "unknown";
    const year_month_day date{year{y}, month{static_cast<unsigned>(mo)}, day{static_cast<unsigned>(d)}};
    if (!date.ok() || h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 || sec > 59)
        return "unknown";
    const auto posted = sys_days{date} + hours{h} + minutes{mi} + seconds{sec};
    const auto age = std::max<std::int64_t>(0, duration_cast<minutes>(now - posted).count());
    if (age >= 24 * 60) return std::to_string(age / (24 * 60)) + "d ago";
    return std::to_string(age / 60) + "h " + std::to_string(age % 60) + "m ago";
}

void wrapped(std::vector<std::string>& lines, std::string_view source, int width,
             std::string_view prefix = {}) {
    if (lines.size() >= 100000) return;
    std::string line = fit(prefix, width);
    const int prefix_width = static_cast<int>(line.size()); // ASCII prefixes only.
    int used = prefix_width;
    bool had = false;
    for (std::size_t at = 0; at < source.size();) {
        char32_t cp = 0;
        std::string_view bytes;
        if (!decode(source, at, cp, bytes)) continue;
        if (cp == '\n') {
            lines.push_back(std::move(line));
            if (lines.size() >= 100000) return;
            line.clear();
            used = 0;
            had = true;
            continue;
        }
        if (cp == '\r' || cp == '\t') { cp = ' '; bytes = " "; }
        if (!printable(cp)) continue;
        const int size = cells(cp);
        if (used + size > width && used > 0) {
            lines.push_back(std::move(line));
            if (lines.size() >= 100000) return;
            line.clear();
            used = 0;
        }
        if (size > width) continue;
        line.append(bytes);
        used += size;
        had = true;
    }
    if (had || !line.empty()) lines.push_back(std::move(line));
    else lines.emplace_back();
}

std::size_t previous_codepoint(std::string_view value, std::size_t at) {
    if (at == 0) return 0;
    --at;
    while (at > 0 && (static_cast<unsigned char>(value[at]) & 0xc0) == 0x80) --at;
    return at;
}

std::size_t next_codepoint(std::string_view value, std::size_t at) {
    if (at >= value.size()) return value.size();
    ++at;
    while (at < value.size() &&
           (static_cast<unsigned char>(value[at]) & 0xc0) == 0x80) ++at;
    return at;
}

struct EditorPoint {
    std::size_t offset;
    int row;
    int column;
};

struct EditorLayout {
    std::vector<std::string> lines{1};
    std::vector<EditorPoint> points{{0, 0, 0}};
};

EditorLayout layout_editor(std::string_view value, int width) {
    EditorLayout layout;
    width = std::max(1, width);
    int column = 0;
    for (std::size_t at = 0; at < value.size();) {
        const std::size_t start = at;
        char32_t cp = 0;
        std::string_view bytes;
        if (!decode(value, at, cp, bytes)) continue;
        if (cp == '\n') {
            layout.lines.emplace_back();
            column = 0;
        } else if (printable(cp)) {
            const int size = cells(cp);
            if (column + size > width && column > 0) {
                layout.lines.emplace_back();
                column = 0;
                // The same byte offset has a position at either end of the wrap.
                layout.points.push_back({start, static_cast<int>(layout.lines.size()) - 1, 0});
            }
            if (size <= width) {
                layout.lines.back().append(bytes);
                column += size;
            }
        }
        layout.points.push_back({at, static_cast<int>(layout.lines.size()) - 1, column});
    }
    return layout;
}

std::size_t point_at(const EditorLayout& layout, std::size_t offset, bool wrap_end) {
    std::size_t first = 0;
    std::size_t last = 0;
    for (std::size_t i = 0; i < layout.points.size(); ++i) {
        if (layout.points[i].offset == offset) {
            first = i;
            last = i;
            while (last + 1 < layout.points.size() && layout.points[last + 1].offset == offset)
                ++last;
            return wrap_end ? first : last;
        }
    }
    return layout.points.size() - 1;
}

} // namespace

Tui::Tui(Store& store) : store_(store) {}

void Tui::set_author_id(std::string author_id) {
    author_id_ = std::move(author_id);
    dirty_ = true;
}

void Tui::resize(int width, int height) {
    width_ = std::clamp(width, 1, 400);
    height_ = std::clamp(height, 1, 200);
    dirty_ = true;
}

std::string Tui::restore_terminal() {
    return "\x1b[?25h\x1b[?1049l";
}

void Tui::refresh_list() {
    try {
        const std::int64_t selected_id = selected_ >= 0 &&
            selected_ < static_cast<int>(threads_.size()) ? threads_[selected_].id : 0;
        threads_ = store_.list_threads();
        selected_ = std::clamp(selected_, 0, std::max(0, static_cast<int>(threads_.size()) - 1));
        for (int i = 0; i < static_cast<int>(threads_.size()); ++i) {
            if (threads_[i].id == selected_id) { selected_ = i; break; }
        }
        status_.clear();
    } catch (const std::exception& error) {
        status_ = std::string("Error: ") + error.what();
    } catch (...) {
        status_ = "Error loading threads";
    }
    dirty_ = true;
}

void Tui::refresh_thread() {
    try {
        auto loaded = store_.get_thread(thread_id_);
        if (loaded) {
            thread_ = std::move(*loaded);
            status_.clear();
        } else {
            status_ = "Thread no longer exists";
            page_ = Page::list;
            refresh_list();
            status_ = "Thread no longer exists";
        }
    } catch (const std::exception& error) {
        status_ = std::string("Error: ") + error.what();
    } catch (...) {
        status_ = "Error loading thread";
    }
    dirty_ = true;
}

void Tui::show_thread(std::int64_t id) {
    thread_id_ = id;
    thread_scroll_ = 0;
    page_ = Page::thread;
    refresh_thread();
}

std::string Tui::start() {
    if (done_) return {};
    if (!started_) {
        started_ = true;
        refresh_list();
        return std::string("\x1b[?1049h\x1b[?25l") + render();
    }
    return render();
}

void Tui::cancel_editor() {
    draft_title_.clear();
    draft_body_.clear();
    editor_cursor_ = 0;
    editor_top_ = 0;
    preferred_column_ = -1;
    editor_wrap_end_ = false;
    page_ = thread_id_ && page_ == Page::reply_body ? Page::thread : Page::list;
    status_ = "Cancelled";
    dirty_ = true;
}

void Tui::submit() {
    if (draft_body_.empty()) { status_ = "Body is empty"; dirty_ = true; return; }
    try {
        if (page_ == Page::new_body) {
            const auto id = store_.create_thread(draft_title_, draft_body_, author_id_);
            draft_title_.clear();
            draft_body_.clear();
            show_thread(id);
            if (page_ == Page::thread) status_ = "Thread posted";
        } else if (page_ == Page::reply_body) {
            store_.reply(thread_id_, draft_body_, author_id_);
            draft_body_.clear();
            page_ = Page::thread;
            refresh_thread();
            thread_scroll_ = 100000;
            if (page_ == Page::thread) status_ = "Reply posted";
        }
    } catch (const std::exception& error) {
        status_ = std::string("Error: ") + error.what();
    } catch (...) {
        status_ = "Error saving post";
    }
    dirty_ = true;
}

void Tui::key(std::string_view name) {
    if (name == "ctrl-c") { done_ = true; return; }
    if (page_ == Page::new_title || page_ == Page::new_body || page_ == Page::reply_body) {
        if (name == "esc") { cancel_editor(); return; }
        std::string& draft = page_ == Page::new_title ? draft_title_ : draft_body_;
        const auto layout = layout_editor(draft, std::max(1, width_ - 1));
        const std::size_t current = point_at(layout, editor_cursor_, editor_wrap_end_);
        const auto set_point = [&](std::size_t index) {
            editor_cursor_ = layout.points[index].offset;
            editor_wrap_end_ = index + 1 < layout.points.size() &&
                layout.points[index + 1].offset == editor_cursor_;
            dirty_ = true;
        };
        if (name == "backspace") {
            const auto left = previous_codepoint(draft, editor_cursor_);
            draft.erase(left, editor_cursor_ - left);
            editor_cursor_ = left;
            editor_wrap_end_ = false;
            preferred_column_ = -1;
            dirty_ = true;
        } else if (name == "delete") {
            draft.erase(editor_cursor_, next_codepoint(draft, editor_cursor_) - editor_cursor_);
            editor_wrap_end_ = false;
            preferred_column_ = -1;
            dirty_ = true;
        } else if (name == "left" || name == "right") {
            editor_cursor_ = name == "left" ? previous_codepoint(draft, editor_cursor_) :
                                                next_codepoint(draft, editor_cursor_);
            editor_wrap_end_ = false;
            preferred_column_ = -1;
            dirty_ = true;
        } else if (name == "home" || name == "end" || name == "ctrl-home" ||
                   name == "ctrl-end") {
            std::size_t target = name == "ctrl-home" ? 0 :
                                 name == "ctrl-end" ? layout.points.size() - 1 : current;
            if (name == "home" || name == "end") {
                const int row = layout.points[current].row;
                for (std::size_t i = 0; i < layout.points.size(); ++i) {
                    if (layout.points[i].row == row) {
                        target = i;
                        if (name == "home") break;
                    }
                }
            }
            set_point(target);
            preferred_column_ = -1;
        } else if (name == "up" || name == "down") {
            if (preferred_column_ < 0) preferred_column_ = layout.points[current].column;
            const int row = std::clamp(layout.points[current].row + (name == "up" ? -1 : 1),
                                       0, static_cast<int>(layout.lines.size()) - 1);
            std::size_t target = current;
            int distance = 1000000;
            for (std::size_t i = 0; i < layout.points.size(); ++i) {
                if (layout.points[i].row != row) continue;
                const int candidate = std::abs(layout.points[i].column - preferred_column_);
                if (candidate < distance) { distance = candidate; target = i; }
            }
            set_point(target);
        } else if (name == "enter") {
            if (page_ == Page::new_title) {
                if (draft_title_.empty()) status_ = "Title is empty";
                else {
                    page_ = Page::new_body; status_.clear();
                    editor_cursor_ = 0; editor_top_ = 0;
                    preferred_column_ = -1; editor_wrap_end_ = false;
                }
            } else if (draft_body_.size() < 16384) {
                draft_body_.insert(editor_cursor_, 1, '\n');
                ++editor_cursor_;
                preferred_column_ = -1;
                editor_wrap_end_ = false;
                status_.clear();
            }
            else status_ = "Body limit: 16384 bytes";
            dirty_ = true;
        } else if (name == "ctrl-d" && page_ != Page::new_title) submit();
        return;
    }
    if (page_ == Page::list) {
        if (name == "q") done_ = true;
        else if (name == "up" || name == "k") {
            selected_ = std::max(0, selected_ - 1); dirty_ = true;
        } else if (name == "down" || name == "j") {
            selected_ = std::min(std::max(0, static_cast<int>(threads_.size()) - 1), selected_ + 1);
            dirty_ = true;
        } else if (name == "enter" || name == "right") {
            if (!threads_.empty()) show_thread(threads_[selected_].id);
        } else if (name == "n") {
            page_ = Page::new_title; draft_title_.clear(); draft_body_.clear();
            editor_cursor_ = 0; editor_top_ = 0;
            preferred_column_ = -1; editor_wrap_end_ = false;
            status_.clear(); dirty_ = true;
        } else if (name == "r") refresh_list();
        else if (name == "page-up") {
            selected_ = std::max(0, selected_ - std::max(1, (height_ - 1) / 3)); dirty_ = true;
        } else if (name == "page-down") {
            selected_ = std::min(std::max(0, static_cast<int>(threads_.size()) - 1),
                                 selected_ + std::max(1, (height_ - 1) / 3)); dirty_ = true;
        }
    } else if (page_ == Page::thread) {
        if (name == "up" || name == "k") { thread_scroll_ = std::max(0, thread_scroll_ - 1); dirty_ = true; }
        else if (name == "down" || name == "j") { thread_scroll_ = std::min(100000, thread_scroll_ + 1); dirty_ = true; }
        else if (name == "page-up") { thread_scroll_ = std::max(0, thread_scroll_ - std::max(1, height_ - 2)); dirty_ = true; }
        else if (name == "page-down") { thread_scroll_ = std::min(100000, thread_scroll_ + std::max(1, height_ - 2)); dirty_ = true; }
        else if (name == "b" || name == "left") { page_ = Page::list; refresh_list(); }
        else if (name == "r") refresh_thread();
        else if (name == "a") {
            page_ = Page::reply_body; draft_body_.clear(); status_.clear();
            editor_cursor_ = 0; editor_top_ = 0;
            preferred_column_ = -1; editor_wrap_end_ = false;
            dirty_ = true;
        }
    }
}

void Tui::character(std::string_view utf8) {
    if (page_ == Page::new_title) {
        if (draft_title_.size() + utf8.size() <= 120) {
            draft_title_.insert(editor_cursor_, utf8);
            editor_cursor_ += utf8.size();
            preferred_column_ = -1; editor_wrap_end_ = false;
            status_.clear(); dirty_ = true;
        } else { status_ = "Title limit: 120 bytes"; dirty_ = true; }
    } else if (page_ == Page::new_body || page_ == Page::reply_body) {
        if (draft_body_.size() + utf8.size() <= 16384) {
            draft_body_.insert(editor_cursor_, utf8);
            editor_cursor_ += utf8.size();
            preferred_column_ = -1; editor_wrap_end_ = false;
            status_.clear(); dirty_ = true;
        } else { status_ = "Body limit: 16384 bytes"; dirty_ = true; }
    } else if (utf8.size() == 1) key(utf8);
}

void Tui::byte(unsigned char value) {
    if (value != '\r' && value != '\n') last_was_cr_ = false;
    if (escape_ == Escape::esc) {
        if (value == '[') {
            escape_ = Escape::csi; escape_data_.clear();
            escape_at_ = std::chrono::steady_clock::now(); return;
        }
        if (value == 'O') {
            escape_ = Escape::ss3; escape_data_.clear();
            escape_at_ = std::chrono::steady_clock::now(); return;
        }
        escape_ = Escape::none;
        key("esc");
    } else if (escape_ == Escape::csi || escape_ == Escape::ss3) {
        if (value >= 0x40 && value <= 0x7e) {
            escape_data_.push_back(static_cast<char>(value));
            const auto sequence = escape_data_;
            escape_data_.clear();
            escape_ = Escape::none;
            if (sequence == "A" || sequence == "1;2A" || sequence == "1;5A") key("up");
            else if (sequence == "B" || sequence == "1;2B" || sequence == "1;5B") key("down");
            else if (sequence == "C" || sequence == "1;2C" || sequence == "1;5C") key("right");
            else if (sequence == "D" || sequence == "1;2D" || sequence == "1;5D") key("left");
            else if (sequence == "H" || sequence == "1~" || sequence == "7~") key("home");
            else if (sequence == "F" || sequence == "4~" || sequence == "8~") key("end");
            else if (sequence == "1;5H" || sequence == "1;5~" || sequence == "7;5~") key("ctrl-home");
            else if (sequence == "1;5F" || sequence == "4;5~" || sequence == "8;5~") key("ctrl-end");
            else if (sequence == "3~") key("delete");
            else if (sequence == "5~") key("page-up");
            else if (sequence == "6~") key("page-down");
            return;
        }
        if (value < 0x20 || value > 0x3f || escape_data_.size() >= 32) {
            escape_ = Escape::none; escape_data_.clear(); return;
        }
        escape_data_.push_back(static_cast<char>(value));
        escape_at_ = std::chrono::steady_clock::now();
        return;
    }
    if (value == 0x1b) {
        utf8_pending_.clear(); utf8_expected_ = 0;
        escape_ = Escape::esc;
        escape_at_ = std::chrono::steady_clock::now();
        return;
    }
    if (value < 0x20 || value == 0x7f) {
        utf8_pending_.clear(); utf8_expected_ = 0;
        if (value == 0x03) key("ctrl-c");
        else if (value == 0x04) key("ctrl-d");
        else if (value == '\r') { key("enter"); last_was_cr_ = true; }
        else if (value == '\n') { if (!last_was_cr_) key("enter"); last_was_cr_ = false; }
        else if (value == 0x08 || value == 0x7f) key("backspace");
        return;
    }
    if (utf8_expected_ > 0) {
        if ((value & 0xc0) == 0x80) {
            utf8_pending_.push_back(static_cast<char>(value));
            if (static_cast<int>(utf8_pending_.size()) == utf8_expected_) {
                std::size_t at = 0; char32_t cp = 0; std::string_view bytes;
                if (decode(utf8_pending_, at, cp, bytes) && printable(cp)) character(bytes);
                utf8_pending_.clear(); utf8_expected_ = 0;
            }
            return;
        }
        utf8_pending_.clear(); utf8_expected_ = 0;
    }
    if (value < 0x80) character(std::string_view(reinterpret_cast<const char*>(&value), 1));
    else if (value >= 0xc2 && value <= 0xdf) { utf8_pending_ = static_cast<char>(value); utf8_expected_ = 2; }
    else if (value >= 0xe0 && value <= 0xef) { utf8_pending_ = static_cast<char>(value); utf8_expected_ = 3; }
    else if (value >= 0xf0 && value <= 0xf4) { utf8_pending_ = static_cast<char>(value); utf8_expected_ = 4; }
}

std::string Tui::input(std::string_view bytes) {
    if (!started_ || done_) return {};
    if (bytes.empty() && escape_ != Escape::none) {
        const auto elapsed = std::chrono::steady_clock::now() - escape_at_;
        if (escape_ == Escape::esc && elapsed >= std::chrono::milliseconds(80)) {
            escape_ = Escape::none;
            key("esc");
        } else if (escape_ != Escape::esc && elapsed >= std::chrono::seconds(1)) {
            escape_ = Escape::none;
            escape_data_.clear();
        }
    }
    for (unsigned char value : bytes) {
        byte(value);
        if (done_) break;
    }
    if (done_) return restore_terminal();
    if (!dirty_) return {};
    return render();
}

std::string Tui::render() {
    dirty_ = false;
    const int content_width = std::max(0, width_ - 1); // Avoid terminal auto-wrap.
    const int content_height = std::max(0, height_ - 2);
    const bool editing = page_ == Page::new_title || page_ == Page::new_body ||
                         page_ == Page::reply_body;
    int cursor_row = 1;
    int cursor_column = 1;
    std::string header;
    std::string footer;
    std::vector<std::string> content;
    if (page_ == Page::list) {
        header = "SSH Forum | Threads (" + std::to_string(threads_.size()) +
                 ") | You: " + display_author(author_id_);
        footer = "j/k: move  PgUp/PgDn: page  Enter/Right: open  n: new  r: refresh  q: quit";
        if (threads_.empty()) content.emplace_back("No threads yet. Press n to post.");
        else {
            const int visible_threads = std::max(1, (content_height + 1) / 3);
            const auto now = std::chrono::system_clock::now();
            if (selected_ < list_top_) list_top_ = selected_;
            if (selected_ >= list_top_ + visible_threads)
                list_top_ = selected_ - visible_threads + 1;
            for (int i = list_top_; i < static_cast<int>(threads_.size()) &&
                 i < list_top_ + visible_threads; ++i) {
                const auto& item = threads_[i];
                content.push_back(aligned(std::string(i == selected_ ? "> " : "  ") + item.title,
                    "Posted: " + relative_time(item.created_at, now), content_width));
                content.push_back(aligned("  [" + std::to_string(item.reply_count) + " replies]",
                    "Last reply: " + (item.last_reply_at.empty() ? std::string("No replies yet") :
                                     relative_time(item.last_reply_at, now)), content_width));
                content.emplace_back();
            }
        }
    } else if (page_ == Page::thread) {
        header = "SSH Forum | Thread #" + std::to_string(thread_id_);
        footer = "j/k: scroll  PgUp/PgDn: page  a: reply  r: refresh  b/Left: back";
        wrapped(content, thread_.summary.title, content_width);
        wrapped(content, "Posted: " + thread_.summary.created_at, content_width);
        wrapped(content, "Last reply: " + (thread_.replies.empty()
            ? std::string("No replies yet") : thread_.replies.back().created_at), content_width);
        wrapped(content, display_author(thread_.summary.author_id), content_width);
        content.emplace_back();
        wrapped(content, thread_.summary.body, content_width);
        for (const auto& reply : thread_.replies) {
            if (content.size() >= 100000) break;
            content.emplace_back(static_cast<std::size_t>(content_width), '-');
            wrapped(content, display_author(reply.author_id) + "  Reply #" +
                             std::to_string(reply.id), content_width);
            wrapped(content, "Posted: " + reply.created_at, content_width);
            wrapped(content, reply.body, content_width);
        }
        thread_scroll_ = std::clamp(thread_scroll_, 0,
            std::max(0, static_cast<int>(content.size()) - content_height));
        if (thread_scroll_ > 0) content.erase(content.begin(), content.begin() + thread_scroll_);
    } else {
        const bool title = page_ == Page::new_title;
        header = title ? "New thread | Title" :
                 page_ == Page::new_body ? "New thread | Body" : "Reply | Body";
        footer = title ? "Arrows: move  Enter: next  Esc: cancel  Max 120 bytes" :
                  "Arrows: move  Enter: newline  Ctrl+D: submit  Esc: cancel  Max 16384 bytes";
        const auto layout = layout_editor(title ? draft_title_ : draft_body_,
                                          std::max(1, content_width));
        const auto& caret = layout.points[point_at(layout, editor_cursor_, editor_wrap_end_)];
        if (content_height > 0) {
            editor_top_ = std::clamp(editor_top_, 0,
                std::max(0, static_cast<int>(layout.lines.size()) - content_height));
            if (caret.row < editor_top_) editor_top_ = caret.row;
            if (caret.row >= editor_top_ + content_height)
                editor_top_ = caret.row - content_height + 1;
            for (int row = editor_top_; row < static_cast<int>(layout.lines.size()) &&
                 static_cast<int>(content.size()) < content_height; ++row)
                content.push_back(layout.lines[row]);
            cursor_row = 2 + caret.row - editor_top_;
            cursor_column = std::clamp(caret.column + 1, 1, width_);
        }
    }
    if (!status_.empty()) footer = status_ + " | " + footer;
    std::string screen = "\x1b[H\x1b[2J";
    for (int row = 0; row < height_; ++row) {
        std::string_view source;
        if (row == 0) source = header;
        else if (row == height_ - 1 && height_ > 1) source = footer;
        else if (row - 1 >= 0 && row - 1 < static_cast<int>(content.size()) &&
                 row - 1 < content_height) source = content[row - 1];
        screen += fit(source, content_width);
        screen += "\x1b[K";
        if (row + 1 < height_) screen += "\r\n";
    }
    if (editing && content_height > 0) {
        screen += "\x1b[" + std::to_string(cursor_row) + ";" +
                  std::to_string(cursor_column) + "H\x1b[?25h";
    } else screen += "\x1b[?25l";
    return screen;
}

} // namespace sshforum
