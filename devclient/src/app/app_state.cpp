#include "client/app/app_state.hpp"

#include <algorithm>
#include <utility>

namespace client::app {

namespace {
std::string trim_right_newline(std::string s) {
    if (!s.empty() && s.back() == '\n') {
        s.pop_back();
    }
    return s;
}
} // namespace

AppState::AppState() = default;

void AppState::set_username(std::string name) {
    if (name.empty()) {
        name = kDefaultUser;
    }
    username_ = std::move(name);
}

void AppState::set_current_room(std::string room) {
    if (room.empty()) {
        room = kDefaultRoom;
    }
    current_room_ = std::move(room);
}

void AppState::set_preview_room(std::string room) {
    preview_room_ = std::move(room);
}

void AppState::update_rooms(std::vector<std::string> rooms,
                            std::vector<bool> locks,
                            const std::string& preferred_room) {
    rooms_ = std::move(rooms);
    rooms_locked_ = std::move(locks);
    if (rooms_locked_.size() != rooms_.size()) {
        rooms_locked_.resize(rooms_.size(), false);
    }
    if (!rooms_.empty()) {
        auto it = std::find(rooms_.begin(), rooms_.end(), preferred_room);
        if (it != rooms_.end()) {
            rooms_selected_ = static_cast<int>(std::distance(rooms_.begin(), it));
        } else {
            rooms_selected_ = std::clamp(rooms_selected_, 0, static_cast<int>(rooms_.size()) - 1);
        }
    } else {
        rooms_selected_ = 0;
    }
}

void AppState::update_users(std::vector<std::string> users) {
    users_ = std::move(users);
    if (users_.empty()) {
        users_.push_back("<none>");
    }
    users_selected_ = std::clamp(users_selected_, 0, static_cast<int>(users_.size()) - 1);
}

void AppState::append_log(std::string line) {
    auto sanitized = trim_right_newline(std::move(line));
    std::lock_guard lk(logs_mu_);
    logs_.emplace_back(std::move(sanitized));
    if (logs_.size() > kMaxLogs) {
        const auto remove = logs_.size() - kMaxLogs;
        logs_.erase(logs_.begin(), logs_.begin() + static_cast<std::ptrdiff_t>(remove));
        log_selected_ = std::max(0, log_selected_ - static_cast<int>(remove));
    }
    if (log_auto_scroll_.load()) {
        log_selected_ = logs_.empty() ? 0 : static_cast<int>(logs_.size()) - 1;
    } else {
        clamp_log_cursor_unlocked();
    }
}

void AppState::clear_logs() {
    std::lock_guard lk(logs_mu_);
    logs_.clear();
    log_selected_ = 0;
}

int AppState::log_selected() const {
    std::lock_guard lk(logs_mu_);
    return log_selected_;
}

void AppState::set_log_selected(int idx) {
    std::lock_guard lk(logs_mu_);
    log_selected_ = std::clamp(idx, 0, logs_.empty() ? 0 : static_cast<int>(logs_.size()) - 1);
    log_auto_scroll_.store(false);
}

void AppState::set_left_panel_width(int width) {
    left_width_ = std::clamp(width, kMinSidebarWidth, kMaxSidebarWidth);
}

void AppState::clamp_log_cursor_unlocked() {
    if (logs_.empty()) {
        log_selected_ = 0;
        return;
    }
    log_selected_ = std::clamp(log_selected_, 0, static_cast<int>(logs_.size()) - 1);
}

} // namespace client::app

