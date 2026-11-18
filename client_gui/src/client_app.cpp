#include "client/client_app.hpp"

#include <algorithm>
#include <chrono>

namespace {

std::uint64_t now_unix_ms() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return ms < 0 ? 0ULL : static_cast<std::uint64_t>(ms);
}

std::string trim_copy(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

void insert_unique_sorted(std::vector<std::string>& items, const std::string& value) {
    if (value.empty()) {
        return;
    }
    if (std::find(items.begin(), items.end(), value) == items.end()) {
        items.push_back(value);
        std::sort(items.begin(), items.end());
    }
}

} // namespace

ClientApp::ClientApp(IClientTransport& transport) : transport_(transport) {
    transport_.set_auto_ping_enabled(data_.diagnostics.auto_ping_enabled);
    setup_callbacks();
}

ClientApp::~ClientApp() {
    disconnect();
}

std::uint64_t ClientApp::current_now_ms() const {
    return active_now_ms_ > 0 ? active_now_ms_ : now_unix_ms();
}

void ClientApp::update(std::uint64_t now_unix_ms_override) {
    struct NowGuard {
        std::uint64_t& slot;
        ~NowGuard() { slot = 0; }
    } now_guard{active_now_ms_};
    active_now_ms_ = (now_unix_ms_override > 0) ? now_unix_ms_override : now_unix_ms();

    event_queue_.process_all();

    if (data_.diagnostics.auto_resume_pending && !data_.is_connected) {
        const std::uint64_t now_ms = current_now_ms();
        const std::uint64_t expires_ms = data_.session.resume_expires_unix_ms;
        if (!data_.diagnostics.auto_resume_enabled || !can_resume()) {
            cancel_auto_resume();
        } else if (expires_ms > 0 && now_ms >= expires_ms) {
            cancel_auto_resume();
            data_.timeline.add_notice("Auto-resume stopped: resume token expired.", now_ms);
            record_protocol_event(ProtocolDirection::LIFECYCLE, "AUTO_RESUME", "stopped (token expired)", now_ms);
        } else if (now_ms >= data_.diagnostics.auto_resume_next_unix_ms) {
            const std::uint32_t current_attempt = data_.diagnostics.auto_resume_attempt;
            data_.diagnostics.auto_resume_pending = false;
            record_protocol_event(
                ProtocolDirection::LIFECYCLE,
                "AUTO_RESUME",
                "attempt=" + std::to_string(current_attempt) + "/" + std::to_string(kAutoResumeMaxAttempts),
                now_ms);
            if (connect(data_.last_host, data_.last_port)) {
                login(last_login_user_, last_access_token_, true);
            } else {
                schedule_auto_resume_retry(now_ms, "connect failed");
            }
        }
    }
}

bool ClientApp::connect(const std::string& host, int port) {
    const std::uint64_t now_ms = current_now_ms();
    if (host.empty() || port <= 0 || port > 65535) {
        data_.connection_state = TransportState::DISCONNECTED;
        data_.is_connected = false;
        data_.timeline.add_error("Invalid server endpoint.", now_ms);
        record_protocol_event(ProtocolDirection::LIFECYCLE, "CONNECT", "invalid endpoint", now_ms);
        return false;
    }

    manual_disconnect_ = false;
    data_.last_host = host;
    data_.last_port = port;
    data_.is_logged_in = false;
    data_.is_admin = false;
    data_.diagnostics.auto_resume_pending = false;
    has_snapshot_room_ = false;
    reset_rtt_stats();
    data_.connection_state = TransportState::CONNECTING;
    data_.is_connected = false;

    if (transport_.connect(host, static_cast<unsigned short>(port))) {
        data_.connection_state = TransportState::CONNECTED;
        data_.is_connected = true;
        record_protocol_event(ProtocolDirection::LIFECYCLE, "CONNECT", host + ":" + std::to_string(port) + " ok", now_ms);
        return true;
    }

    data_.connection_state = TransportState::DISCONNECTED;
    data_.is_connected = false;
    data_.timeline.add_error("Failed to connect to " + host + ":" + std::to_string(port), now_ms);
    record_protocol_event(ProtocolDirection::LIFECYCLE, "CONNECT", host + ":" + std::to_string(port) + " failed", now_ms);
    return false;
}

void ClientApp::disconnect() {
    const bool was_active = data_.is_connected || data_.diagnostics.auto_resume_pending;
    manual_disconnect_ = true;
    cancel_auto_resume();
    transport_.close();
    data_.connection_state = TransportState::DISCONNECTED;
    data_.is_connected = false;
    data_.is_logged_in = false;
    data_.is_admin = false;
    has_snapshot_room_ = false;
    reset_rtt_stats();
    if (was_active) {
        record_protocol_event(ProtocolDirection::LIFECYCLE, "DISCONNECT", "manual disconnect");
    }
}

void ClientApp::login(const std::string& username, const std::string& access_token, bool resume) {
    last_login_user_ = username;
    last_access_token_ = access_token;

    std::string credentials = access_token;
    if (resume && !resume_token_secret_.empty()) {
        credentials += access_token.empty() ? "resume:" : "|resume:";
        credentials += resume_token_secret_;
    }
    record_protocol_event(
        ProtocolDirection::TX,
        "MSG_LOGIN_REQ",
        "user=" + username +
            " auth=" + (access_token.empty() ? std::string("none") : std::string("token")) +
            " resume=" + ((resume && !resume_token_secret_.empty()) ? std::string("true") : std::string("false")));
    transport_.send_login(username, credentials);
}

bool ClientApp::reconnect_and_login(const std::string& host,
                                    int port,
                                    const std::string& username,
                                    const std::string& access_token,
                                    bool resume) {
    disconnect();
    if (!connect(host, port)) {
        return false;
    }
    login(username, access_token, resume);
    return true;
}

void ClientApp::process_command(const std::string& input) {
    if (input.empty()) {
        return;
    }

    if (input[0] == '/') {
        const size_t split = input.find(' ');
        const std::string cmd = (split == std::string::npos) ? input : input.substr(0, split);
        const std::string args = (split == std::string::npos) ? "" : trim_copy(input.substr(split + 1));

        if (cmd == "/join") {
            if (!args.empty()) {
                const size_t pass_split = args.find(' ');
                if (pass_split != std::string::npos) {
                    const std::string room = args.substr(0, pass_split);
                    const std::string pass = trim_copy(args.substr(pass_split + 1));
                    join_room(room, pass);
                } else {
                    join_room(args, "");
                }
            } else {
                data_.timeline.add_notice("usage: /join <room> [password]", now_unix_ms());
            }
            return;
        }

        if (cmd == "/leave") {
            leave_current_room();
            return;
        }

        if (cmd == "/w" || cmd == "/whisper") {
            const size_t msg_split = args.find(' ');
            if (msg_split != std::string::npos) {
                const std::string target = args.substr(0, msg_split);
                const std::string msg = trim_copy(args.substr(msg_split + 1));
                if (!target.empty() && !msg.empty()) {
                    whisper(target, msg);
                    return;
                }
            }
            data_.timeline.add_notice("usage: /w <user> <message>", now_unix_ms());
            return;
        }

        if (cmd == "/refresh") {
            request_refresh();
            return;
        }

        if (cmd == "/rooms") {
            if (data_.is_connected && data_.is_logged_in) {
                record_protocol_event(ProtocolDirection::TX, "MSG_ROOMS_REQ", "room=" + data_.current_room);
                transport_.send_rooms(data_.current_room);
            } else {
                data_.timeline.add_error("Not connected or not logged in", current_now_ms());
            }
            return;
        }

        if (cmd == "/who") {
            if (data_.is_connected && data_.is_logged_in) {
                const std::string target_room = args.empty() ? data_.current_room : args;
                record_protocol_event(ProtocolDirection::TX, "MSG_ROOM_USERS_REQ", "room=" + target_room);
                transport_.send_who(target_room);
            } else {
                data_.timeline.add_error("Not connected or not logged in", current_now_ms());
            }
            return;
        }

        if (data_.is_connected && data_.is_logged_in) {
            record_protocol_event(
                ProtocolDirection::TX,
                "MSG_CHAT_SEND",
                "room=" + data_.current_room + " cmd=" + cmd);
            transport_.send_chat(data_.current_room, input);
        } else {
            data_.timeline.add_error("Not connected or not logged in", current_now_ms());
        }
        return;
    }

    if (data_.is_connected && data_.is_logged_in) {
        const std::string clamped = truncate_utf8_to_char_units(input, kMaxChatCharUnits);
        if (!clamped.empty()) {
            record_protocol_event(
                ProtocolDirection::TX,
                "MSG_CHAT_SEND",
                "room=" + data_.current_room + " bytes=" + std::to_string(clamped.size()));
            transport_.send_chat(data_.current_room, clamped);
        }
    } else {
        data_.timeline.add_error("Not connected or not logged in", current_now_ms());
    }
}

void ClientApp::join_room(const std::string& room_name, const std::string& password) {
    if (!data_.is_connected || !data_.is_logged_in) {
        data_.timeline.add_error("Not connected or not logged in", current_now_ms());
        return;
    }
    record_protocol_event(
        ProtocolDirection::TX,
        "MSG_JOIN_ROOM",
        "room=" + room_name + " locked=" + (password.empty() ? std::string("false") : std::string("true")));
    transport_.send_join(room_name, password);
}

void ClientApp::leave_current_room() {
    if (!data_.is_connected || !data_.is_logged_in) {
        data_.timeline.add_error("Not connected or not logged in", current_now_ms());
        return;
    }
    if (data_.current_room != "lobby") {
        record_protocol_event(ProtocolDirection::TX, "MSG_LEAVE_ROOM", "room=" + data_.current_room);
        transport_.send_leave(data_.current_room);
    }
}

void ClientApp::whisper(const std::string& target, const std::string& message) {
    if (!data_.is_connected || !data_.is_logged_in) {
        data_.timeline.add_error("Not connected or not logged in", current_now_ms());
        return;
    }
    const std::string clamped = truncate_utf8_to_char_units(message, kMaxChatCharUnits);
    if (clamped.empty()) {
        return;
    }
    record_protocol_event(
        ProtocolDirection::TX,
        "MSG_WHISPER_REQ",
        "target=" + target + " bytes=" + std::to_string(clamped.size()));
    transport_.send_whisper(target, clamped);
}

void ClientApp::block_user(const std::string& target) {
    const std::string clean = trim_copy(target);
    if (!clean.empty()) {
        mark_whisper_peer_read(clean);
        process_command("/block " + clean);
    }
}

void ClientApp::unblock_user(const std::string& target) {
    const std::string clean = trim_copy(target);
    if (!clean.empty()) {
        process_command("/unblock " + clean);
    }
}

void ClientApp::request_blacklist() {
    process_command("/blacklist list");
}

bool ClientApp::is_user_blocked(const std::string& target) const {
    return std::find(data_.blocked_users.begin(), data_.blocked_users.end(), target) != data_.blocked_users.end();
}

void ClientApp::request_refresh() {
    if (!data_.is_connected || !data_.is_logged_in) {
        data_.timeline.add_error("Not connected or not logged in", current_now_ms());
        return;
    }
    record_protocol_event(ProtocolDirection::TX, "MSG_REFRESH_REQ", "room=" + data_.current_room);
    transport_.send_refresh(data_.current_room);
}

void ClientApp::request_ping() {
    if (data_.is_connected) {
        record_protocol_event(ProtocolDirection::TX, "MSG_PING", "manual keepalive");
        transport_.send_ping();
    }
}

bool ClientApp::can_resume() const {
    return data_.session.has_resume_token && !resume_token_secret_.empty();
}

void ClientApp::clear_saved_resume_token() {
    resume_token_secret_.clear();
    data_.session.has_resume_token = false;
    data_.session.resume_expires_unix_ms = 0;
    data_.session.resumed = false;
    cancel_auto_resume();
}

void ClientApp::set_auto_resume_enabled(bool enabled) {
    data_.diagnostics.auto_resume_enabled = enabled;
    if (!enabled) {
        cancel_auto_resume();
    }
}

void ClientApp::cancel_auto_resume() {
    const bool was_pending = data_.diagnostics.auto_resume_pending;
    data_.diagnostics.auto_resume_pending = false;
    data_.diagnostics.auto_resume_attempt = 0;
    data_.diagnostics.auto_resume_next_unix_ms = 0;
    if (was_pending) {
        record_protocol_event(ProtocolDirection::LIFECYCLE, "AUTO_RESUME", "cancelled");
    }
}

void ClientApp::set_auto_ping_enabled(bool enabled) {
    data_.diagnostics.auto_ping_enabled = enabled;
    transport_.set_auto_ping_enabled(enabled);
}

void ClientApp::clear_protocol_events() {
    data_.protocol_events.clear();
}

void ClientApp::record_rtt_sample(std::uint32_t rtt_ms) {
    rtt_samples_.push_back(rtt_ms);
    if (rtt_samples_.size() > 32) {
        rtt_samples_.pop_front();
    }
    if (!data_.diagnostics.has_rtt) {
        data_.diagnostics.min_rtt_ms = rtt_ms;
        data_.diagnostics.max_rtt_ms = rtt_ms;
    } else {
        data_.diagnostics.min_rtt_ms = std::min(data_.diagnostics.min_rtt_ms, rtt_ms);
        data_.diagnostics.max_rtt_ms = std::max(data_.diagnostics.max_rtt_ms, rtt_ms);
    }
    std::uint64_t sum = 0;
    for (std::uint32_t sample : rtt_samples_) {
        sum += sample;
    }
    data_.diagnostics.last_rtt_ms = rtt_ms;
    data_.diagnostics.avg_rtt_ms =
        static_cast<std::uint32_t>((sum + rtt_samples_.size() / 2) / rtt_samples_.size());
    ++data_.diagnostics.rtt_sample_count;
    data_.diagnostics.has_rtt = true;
}

void ClientApp::reset_rtt_stats() {
    rtt_samples_.clear();
    data_.diagnostics.has_rtt = false;
    data_.diagnostics.last_rtt_ms = 0;
    data_.diagnostics.min_rtt_ms = 0;
    data_.diagnostics.max_rtt_ms = 0;
    data_.diagnostics.avg_rtt_ms = 0;
    data_.diagnostics.rtt_sample_count = 0;
}

void ClientApp::schedule_auto_resume_retry(std::uint64_t now_ms, const std::string& reason) {
    if (manual_disconnect_ || !data_.diagnostics.auto_resume_enabled || !can_resume()) {
        return;
    }
    const std::uint64_t expires_ms = data_.session.resume_expires_unix_ms;
    if (expires_ms > 0 && now_ms >= expires_ms) {
        cancel_auto_resume();
        data_.timeline.add_notice("Auto-resume unavailable: resume token expired.", now_ms);
        record_protocol_event(ProtocolDirection::LIFECYCLE, "AUTO_RESUME", "stopped (token expired)", now_ms);
        return;
    }
    if (data_.diagnostics.auto_resume_attempt >= kAutoResumeMaxAttempts) {
        cancel_auto_resume();
        data_.timeline.add_error(
            "Auto-resume exhausted after " + std::to_string(kAutoResumeMaxAttempts) + " attempts.",
            now_ms);
        record_protocol_event(ProtocolDirection::LIFECYCLE, "AUTO_RESUME", "exhausted", now_ms);
        return;
    }

    ++data_.diagnostics.auto_resume_attempt;
    const std::uint32_t attempt = data_.diagnostics.auto_resume_attempt;
    const std::uint64_t delay_ms = 1000ULL << (attempt - 1u);
    data_.diagnostics.auto_resume_pending = true;
    data_.diagnostics.auto_resume_next_unix_ms = now_ms + delay_ms;

    data_.timeline.add_notice(
        "Auto-resume scheduled (#" + std::to_string(attempt) + "/" +
            std::to_string(kAutoResumeMaxAttempts) + " in " + std::to_string(delay_ms / 1000ULL) + "s)",
        now_ms);
    record_protocol_event(
        ProtocolDirection::LIFECYCLE,
        "AUTO_RESUME",
        reason + " -> retry #" + std::to_string(attempt) + " in " + std::to_string(delay_ms) + "ms",
        now_ms);
}

void ClientApp::record_protocol_event(ProtocolDirection direction,
                                      std::string opcode,
                                      std::string summary,
                                      std::uint64_t ts_ms) {
    ProtocolEventEntry entry;
    entry.timestamp_ms = (ts_ms > 0) ? ts_ms : current_now_ms();
    entry.direction = direction;
    entry.opcode = std::move(opcode);
    entry.summary = std::move(summary);
    data_.protocol_events.push_back(std::move(entry));
    while (data_.protocol_events.size() > kMaxProtocolEvents) {
        data_.protocol_events.pop_front();
    }
}

void ClientApp::mark_whisper_peer_read(const std::string& peer) {
    const auto it = data_.whisper_unread_by_peer.find(peer);
    if (it == data_.whisper_unread_by_peer.end()) {
        return;
    }
    const std::uint32_t count = it->second;
    data_.total_unread_whispers = (data_.total_unread_whispers > count) ? (data_.total_unread_whispers - count) : 0;
    data_.whisper_unread_by_peer.erase(it);
}

void ClientApp::mark_all_whispers_read() {
    data_.whisper_unread_by_peer.clear();
    data_.total_unread_whispers = 0;
}

std::uint32_t ClientApp::unread_whisper_count(const std::string& peer) const {
    const auto it = data_.whisper_unread_by_peer.find(peer);
    return (it != data_.whisper_unread_by_peer.end()) ? it->second : 0;
}

void ClientApp::sync_blacklist_from_notice(const std::string& text) {
    if (text == "blacklist: (empty)") {
        data_.blocked_users.clear();
        return;
    }

    constexpr const char* kListPrefix = "blacklist: ";
    if (text.rfind(kListPrefix, 0) == 0) {
        data_.blocked_users.clear();
        std::string rest = text.substr(11);
        std::size_t start = 0;
        while (start <= rest.size()) {
            const std::size_t comma = rest.find(',', start);
            const std::string item = trim_copy(
                comma == std::string::npos ? rest.substr(start) : rest.substr(start, comma - start));
            insert_unique_sorted(data_.blocked_users, item);
            if (!item.empty()) {
                mark_whisper_peer_read(item);
            }
            if (comma == std::string::npos) {
                break;
            }
            start = comma + 1;
        }
        return;
    }

    constexpr const char* kAddPrefix = "blacklist add: ";
    if (text.rfind(kAddPrefix, 0) == 0) {
        const std::string item = trim_copy(text.substr(15));
        insert_unique_sorted(data_.blocked_users, item);
        if (!item.empty()) {
            mark_whisper_peer_read(item);
        }
        return;
    }

    constexpr const char* kRemovePrefix = "blacklist remove: ";
    if (text.rfind(kRemovePrefix, 0) == 0) {
        const std::string target = trim_copy(text.substr(18));
        std::erase(data_.blocked_users, target);
    }
}

void ClientApp::record_whisper_peer(const std::string& peer) {
    if (peer.empty()) {
        return;
    }
    std::erase(data_.whisper_peers, peer);
    data_.whisper_peers.insert(data_.whisper_peers.begin(), peer);
    if (data_.whisper_peers.size() > 20) {
        data_.whisper_peers.resize(20);
    }
}

void ClientApp::setup_callbacks() {
    transport_.set_on_hello([this](std::uint16_t caps) {
        event_queue_.push([this, caps]() {
            const std::uint64_t ts = current_now_ms();
            data_.diagnostics.server_caps = caps;
            data_.timeline.add_notice("Server hello received (caps=" + std::to_string(caps) + ")", ts);
            record_protocol_event(ProtocolDirection::RX, "MSG_HELLO", "caps=" + std::to_string(caps), ts);
        });
    });

    transport_.set_on_err([this](std::uint16_t code, std::string msg) {
        event_queue_.push([this, code, msg = std::move(msg)]() mutable {
            const std::uint64_t ts = current_now_ms();
            data_.diagnostics.last_error_code = code;
            data_.diagnostics.last_error_message = msg;
            if (!data_.is_logged_in &&
                (data_.diagnostics.auto_resume_pending || data_.diagnostics.auto_resume_attempt > 0)) {
                cancel_auto_resume();
            }
            data_.timeline.add_error("[" + std::to_string(code) + "] " + msg, ts);
            record_protocol_event(ProtocolDirection::RX, "MSG_ERR", "code=" + std::to_string(code) + " " + msg, ts);
        });
    });

    transport_.set_on_session_info([this](TransportSessionInfo info) {
        event_queue_.push([this, info = std::move(info)]() mutable {
            resume_token_secret_ = std::move(info.resume_token);
            data_.session.principal_id = std::move(info.principal_id);
            data_.session.auth_session_id = std::move(info.auth_session_id);
            data_.session.display_name = std::move(info.display_name);
            data_.session.logical_session_id = std::move(info.logical_session_id);
            data_.session.resume_expires_unix_ms = info.resume_expires_unix_ms;
            data_.session.resumed = info.resumed;
            data_.session.has_resume_token = !resume_token_secret_.empty();
        });
    });

    transport_.set_on_login_res([this](std::string effective_user, std::uint32_t sid, bool is_admin) {
        event_queue_.push([this, effective_user = std::move(effective_user), sid, is_admin]() mutable {
            const std::uint64_t ts = current_now_ms();
            data_.is_logged_in = true;
            data_.is_admin = is_admin;
            data_.session.effective_user = effective_user;
            data_.session.session_id = sid;
            cancel_auto_resume();
            std::string notice = "Logged in as " + effective_user + " (SID " + std::to_string(sid);
            if (data_.session.resumed) {
                notice += ", resumed";
            }
            notice += ")";
            data_.timeline.add_notice(std::move(notice), ts);
            record_protocol_event(
                ProtocolDirection::RX,
                "MSG_LOGIN_RES",
                "user=" + effective_user +
                    " sid=" + std::to_string(sid) +
                    " admin=" + (is_admin ? std::string("true") : std::string("false")) +
                    " resumed=" + (data_.session.resumed ? std::string("true") : std::string("false")),
                ts);
        });
    });

    transport_.set_on_state_changed([this](TransportState state) {
        event_queue_.push([this, state]() {
            data_.connection_state = state;
            data_.is_connected = (state == TransportState::CONNECTED);
            if (state == TransportState::DISCONNECTED) {
                data_.is_logged_in = false;
                data_.is_admin = false;
                has_snapshot_room_ = false;
                reset_rtt_stats();
            }
        });
    });

    transport_.set_on_pong([this](std::uint32_t rtt_ms) {
        event_queue_.push([this, rtt_ms]() {
            const std::uint64_t ts = current_now_ms();
            record_rtt_sample(rtt_ms);
            record_protocol_event(
                ProtocolDirection::RX,
                "MSG_PONG",
                "rtt=" + std::to_string(rtt_ms) + "ms avg=" + std::to_string(data_.diagnostics.avg_rtt_ms) + "ms",
                ts);
        });
    });

    transport_.set_on_disconnected([this](std::string reason) {
        event_queue_.push([this, reason = std::move(reason)]() mutable {
            const std::uint64_t ts = current_now_ms();
            data_.connection_state = TransportState::DISCONNECTED;
            data_.is_connected = false;
            data_.is_logged_in = false;
            data_.is_admin = false;
            has_snapshot_room_ = false;
            reset_rtt_stats();
            data_.diagnostics.last_disconnect_reason = reason;
            data_.timeline.add_notice("Disconnected: " + reason, ts);
            record_protocol_event(ProtocolDirection::LIFECYCLE, "DISCONNECT", "reason=" + reason, ts);
            schedule_auto_resume_retry(ts, "disconnected");
        });
    });

    transport_.set_on_broadcast([this](std::string room,
                                       std::string sender,
                                       std::string text,
                                       std::uint16_t,
                                       std::uint32_t,
                                       std::uint64_t id,
                                       std::string room_id,
                                       std::uint64_t ts_ms) {
        event_queue_.push([this,
                           room = std::move(room),
                           sender = std::move(sender),
                           text = std::move(text),
                           id,
                           room_id = std::move(room_id),
                           ts_ms]() mutable {
            const std::uint64_t effective_ts = (ts_ms > 0) ? ts_ms : current_now_ms();
            const bool is_system_notice = (id == 0 || room_id.empty() || room == "(system)" || sender == "(system)");
            if (is_system_notice) {
                sync_blacklist_from_notice(text);
                record_protocol_event(ProtocolDirection::RX, "MSG_CHAT_BROADCAST", "system notice", effective_ts);
                data_.timeline.add_notice(std::move(text), effective_ts);
                return;
            }
            record_protocol_event(
                ProtocolDirection::RX,
                "MSG_CHAT_BROADCAST",
                "room=" + room + " id=" + std::to_string(static_cast<unsigned long long>(id)) + " sender=" + sender,
                effective_ts);
            data_.timeline.add_chat_message(room_id, room, id, std::move(sender), std::move(text), effective_ts);
        });
    });

    transport_.set_on_room_users([this](std::string room, std::vector<std::string> users) {
        event_queue_.push([this, room = std::move(room), users = std::move(users)]() mutable {
            const std::uint64_t ts = current_now_ms();
            record_protocol_event(
                ProtocolDirection::RX,
                "MSG_ROOM_USERS",
                "room=" + room + " count=" + std::to_string(users.size()),
                ts);
            if (room == data_.current_room) {
                const std::size_t count = users.size();
                data_.user_list = std::move(users);
                data_.timeline.add_notice("Room users updated (" + std::to_string(count) + ")", ts);
            }
        });
    });

    transport_.set_on_snapshot([this](std::string cur_room,
                                      std::vector<std::string> rooms,
                                      std::vector<std::string> users,
                                      std::vector<bool> locked,
                                      std::vector<SnapshotMessage> msgs,
                                      std::string,
                                      std::uint64_t watermark,
                                      std::string room_id,
                                      bool history_available) {
        event_queue_.push([this,
                           cur_room = std::move(cur_room),
                           rooms = std::move(rooms),
                           users = std::move(users),
                           locked = std::move(locked),
                           msgs = std::move(msgs),
                           watermark,
                           room_id = std::move(room_id),
                           history_available]() mutable {
            const std::uint64_t ts = current_now_ms();
            const bool room_changed = (!has_snapshot_room_ ||
                                       data_.current_room != cur_room ||
                                       data_.diagnostics.current_room_id != room_id);
            data_.current_room = cur_room;
            has_snapshot_room_ = true;

            data_.timeline.snapshot(cur_room, room_id, watermark);
            data_.diagnostics.current_room_id = room_id;
            data_.diagnostics.current_watermark = std::max(data_.timeline.watermark_for(room_id), watermark);
            data_.diagnostics.history_available = history_available;

            record_protocol_event(
                ProtocolDirection::RX,
                "MSG_STATE_SNAPSHOT",
                "room=" + cur_room +
                    " wm=" + std::to_string(static_cast<unsigned long long>(watermark)) +
                    " msgs=" + std::to_string(msgs.size()) +
                    " hist=" + (history_available ? std::string("true") : std::string("false")),
                ts);

            if (!history_available) {
                data_.timeline.add_notice("Recent room history is partial (cache fallback or history unavailable).", ts);
            }

            data_.room_list.clear();
            data_.room_list.reserve(rooms.size());
            for (size_t i = 0; i < rooms.size(); ++i) {
                const bool is_locked = (i < locked.size()) ? locked[i] : false;
                data_.room_list.push_back({std::move(rooms[i]), is_locked});
            }
            data_.user_list = std::move(users);

            if (room_changed) {
                data_.timeline.add_notice("Joined room: " + cur_room, ts);
            }
            for (const auto& m : msgs) {
                data_.timeline.add_chat_message(room_id, cur_room, m.id, m.sender, m.text, m.ts_ms);
            }
        });
    });

    transport_.set_on_whisper([this](std::string sender, std::string recipient, std::string text, bool outgoing) {
        event_queue_.push([this,
                           sender = std::move(sender),
                           recipient = std::move(recipient),
                           text = std::move(text),
                           outgoing]() mutable {
            const std::uint64_t ts = current_now_ms();
            record_whisper_peer(outgoing ? recipient : sender);
            if (!outgoing && !sender.empty() && !is_user_blocked(sender)) {
                ++data_.whisper_unread_by_peer[sender];
                ++data_.total_unread_whispers;
            }
            record_protocol_event(
                ProtocolDirection::RX,
                "MSG_WHISPER_BROADCAST",
                outgoing ? ("out -> " + recipient) : ("in <- " + sender),
                ts);
            data_.timeline.add_whisper(std::move(sender), std::move(recipient), std::move(text), outgoing, ts);
        });
    });

    transport_.set_on_whisper_result([this](bool success, std::string reason) {
        event_queue_.push([this, success, reason = std::move(reason)]() mutable {
            const std::uint64_t ts = current_now_ms();
            record_protocol_event(
                ProtocolDirection::RX,
                "MSG_WHISPER_RES",
                "ok=" + (success ? std::string("true") : std::string("false")) +
                    (reason.empty() ? std::string() : (" (" + reason + ")")),
                ts);
            if (!success) {
                data_.timeline.add_error("Whisper failed: " + reason, ts);
            }
        });
    });

    transport_.set_on_refresh_notify([this]() {
        event_queue_.push([this]() {
            const std::uint64_t ts = current_now_ms();
            record_protocol_event(ProtocolDirection::RX, "MSG_REFRESH_NOTIFY", "room=" + data_.current_room, ts);
            if (data_.is_connected && data_.is_logged_in) {
                record_protocol_event(ProtocolDirection::TX, "MSG_REFRESH_REQ", "room=" + data_.current_room, ts);
                transport_.send_refresh(data_.current_room);
            }
        });
    });
}
