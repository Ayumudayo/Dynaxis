#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace client::app {

class AppState {
public:
    static constexpr std::size_t kMaxLogs = 1000;
    static constexpr int kMinSidebarWidth = 16;
    static constexpr int kMaxSidebarWidth = 60;
    static constexpr const char* kDefaultRoom = "lobby";
    static constexpr const char* kDefaultUser = "guest";
    static constexpr const char* kLockIcon = "\xF0\x9F\x94\x92";

    AppState();

    // connection state
    bool connected() const { return connected_.load(); }
    void set_connected(bool value) { connected_.store(value); }

    std::uint32_t session_id() const { return my_sid_; }
    void set_session_id(std::uint32_t sid) { my_sid_ = sid; }

    bool sender_sid_supported() const { return cap_sender_sid_; }
    void set_sender_sid_supported(bool value) { cap_sender_sid_ = value; }

    // user/session info
    const std::string& username() const { return username_; }
    void set_username(std::string name);

    const std::string& current_room() const { return current_room_; }
    void set_current_room(std::string room);

    const std::string& preview_room() const { return preview_room_; }
    void set_preview_room(std::string room);

    const std::string& pending_join_room() const { return pending_join_room_; }
    void set_pending_join_room(std::string room) { pending_join_room_ = std::move(room); }
    void clear_pending_join_room() { pending_join_room_.clear(); }

    // rooms/users
    std::vector<std::string>& rooms() { return rooms_; }
    const std::vector<std::string>& rooms() const { return rooms_; }
    std::vector<bool>& rooms_locked() { return rooms_locked_; }
    const std::vector<bool>& rooms_locked() const { return rooms_locked_; }
    int& rooms_selected() { return rooms_selected_; }
    int rooms_selected() const { return rooms_selected_; }
    void update_rooms(std::vector<std::string> rooms, std::vector<bool> locks, const std::string& preferred_room);

    std::vector<std::string>& users() { return users_; }
    const std::vector<std::string>& users() const { return users_; }
    int& users_selected() { return users_selected_; }
    int users_selected() const { return users_selected_; }
    void update_users(std::vector<std::string> users);

    // logs
    void append_log(std::string line);
    void clear_logs();
    std::vector<std::string>& mutable_logs() { return logs_; }
    const std::vector<std::string>& logs() const { return logs_; }
    int& log_selected_ref() { return log_selected_; }
    int log_selected() const;
    void set_log_selected(int idx);
    bool log_auto_scroll() const { return log_auto_scroll_.load(); }
    void set_log_auto_scroll(bool value) { log_auto_scroll_.store(value); }

    // UI helpers
    std::string& input_buffer() { return input_; }
    const std::string& input_buffer() const { return input_; }

    bool show_help() const { return show_help_; }
    void toggle_help() { show_help_ = !show_help_; }
    void set_help(bool value) { show_help_ = value; }

    int left_panel_width() const { return left_width_; }
    void set_left_panel_width(int width);

private:
    void clamp_log_cursor_unlocked();

    std::atomic<bool> connected_{false};
    std::atomic<bool> log_auto_scroll_{true};
    std::uint32_t my_sid_{0};
    bool cap_sender_sid_{false};

    std::string username_{kDefaultUser};
    std::string current_room_{kDefaultRoom};
    std::string preview_room_;
    std::string pending_join_room_;

    std::vector<std::string> rooms_;
    std::vector<bool> rooms_locked_;
    int rooms_selected_{0};

    std::vector<std::string> users_{"(none)"};
    int users_selected_{0};

    mutable std::mutex logs_mu_;
    std::vector<std::string> logs_;
    int log_selected_{0};

    std::string input_;
    int left_width_{26};
    bool show_help_{false};
};

} // namespace client::app
