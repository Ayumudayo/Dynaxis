#include <gtest/gtest.h>

#include "client/client_app.hpp"
#include "client/client_preferences.hpp"
#include "client/message_timeline.hpp"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace {

class FakeClientTransport final : public IClientTransport {
public:
    struct LoginCall {
        std::string user;
        std::string token;
    };
    struct JoinCall {
        std::string room;
        std::string password;
    };
    struct ChatCall {
        std::string room;
        std::string text;
    };
    struct WhisperCall {
        std::string user;
        std::string text;
    };

    bool connect_result{true};
    bool connected{false};
    bool auto_ping_enabled{true};
    int connect_calls{0};
    int close_calls{0};
    int ping_calls{0};
    std::vector<LoginCall> logins;
    std::vector<JoinCall> joins;
    std::vector<std::string> leaves;
    std::vector<ChatCall> chats;
    std::vector<std::string> refreshes;
    std::vector<std::string> who_requests;
    std::vector<std::string> rooms_requests;
    std::vector<WhisperCall> whispers;

    OnHello on_hello;
    OnErr on_err;
    OnLoginRes on_login_res;
    OnSessionInfo on_session_info;
    OnBroadcast on_broadcast;
    OnRoomUsers on_room_users;
    OnSnapshot on_snapshot;
    OnWhisper on_whisper;
    OnWhisperResult on_whisper_result;
    OnRefreshNotify on_refresh_notify;
    OnDisconnected on_disconnected;
    OnStateChanged on_state_changed;
    OnPong on_pong;

    void set_on_hello(OnHello f) override { on_hello = std::move(f); }
    void set_on_err(OnErr f) override { on_err = std::move(f); }
    void set_on_login_res(OnLoginRes f) override { on_login_res = std::move(f); }
    void set_on_session_info(OnSessionInfo f) override { on_session_info = std::move(f); }
    void set_on_broadcast(OnBroadcast f) override { on_broadcast = std::move(f); }
    void set_on_room_users(OnRoomUsers f) override { on_room_users = std::move(f); }
    void set_on_snapshot(OnSnapshot f) override { on_snapshot = std::move(f); }
    void set_on_whisper(OnWhisper f) override { on_whisper = std::move(f); }
    void set_on_whisper_result(OnWhisperResult f) override { on_whisper_result = std::move(f); }
    void set_on_refresh_notify(OnRefreshNotify f) override { on_refresh_notify = std::move(f); }
    void set_on_disconnected(OnDisconnected f) override { on_disconnected = std::move(f); }
    void set_on_state_changed(OnStateChanged f) override { on_state_changed = std::move(f); }
    void set_on_pong(OnPong f) override { on_pong = std::move(f); }

    bool connect(const std::string&, unsigned short) override {
        ++connect_calls;
        if (on_state_changed) {
            on_state_changed(TransportState::CONNECTING);
        }
        if (!connect_result) {
            connected = false;
            if (on_state_changed) {
                on_state_changed(TransportState::DISCONNECTED);
            }
            return false;
        }
        connected = true;
        if (on_state_changed) {
            on_state_changed(TransportState::CONNECTED);
        }
        return true;
    }

    void close() override {
        ++close_calls;
        const bool was_connected = connected;
        connected = false;
        if (was_connected && on_state_changed) {
            on_state_changed(TransportState::DISCONNECTED);
        }
    }

    bool is_connected() const override { return connected; }

    void send_login(const std::string& user, const std::string& token) override {
        logins.push_back({user, token});
    }

    void send_join(const std::string& room, const std::string& password = std::string()) override {
        joins.push_back({room, password});
    }

    void send_leave(const std::string& room = std::string()) override {
        leaves.push_back(room);
    }

    void send_chat(const std::string& room, const std::string& text) override {
        chats.push_back({room, text});
    }

    void send_refresh(const std::string& current_room) override {
        refreshes.push_back(current_room);
    }

    void send_who(const std::string& room) override {
        who_requests.push_back(room);
    }

    void send_rooms(const std::string& current_room) override {
        rooms_requests.push_back(current_room);
    }

    void send_whisper(const std::string& user, const std::string& text) override {
        whispers.push_back({user, text});
    }

    void send_ping() override {
        ++ping_calls;
    }

    void set_auto_ping_enabled(bool enabled) override {
        auto_ping_enabled = enabled;
    }
};

bool timeline_contains(const MessageTimeline& timeline, const std::string& needle) {
    for (const auto& line : timeline.lines) {
        if (line.text.find(needle) != std::string::npos ||
            line.sender.find(needle) != std::string::npos ||
            line.recipient.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST(MessageTimelineTest, SnapshotLiveOverlapAndLateArrivalMergeById) {
    MessageTimeline timeline;
    timeline.add_message("room-id", 30, "third");
    timeline.snapshot("room", "room-id", 30);
    timeline.add_message("room-id", 20, "second");
    timeline.add_message("room-id", 30, "third duplicate");
    timeline.add_message("room-id", 10, "first delayed");
    ASSERT_EQ(timeline.lines.size(), 3);
    EXPECT_EQ(timeline.lines[0].message_id, 10);
    EXPECT_EQ(timeline.lines[1].message_id, 20);
    EXPECT_EQ(timeline.lines[2].text, "third");
}

TEST(MessageTimelineTest, NewRoomGenerationDoesNotKeepOldHistory) {
    MessageTimeline timeline;
    timeline.snapshot("room", "old", 5);
    timeline.add_message("old", 5, "old message");
    timeline.snapshot("room", "new", 0);
    timeline.add_message("new", 6, "new message");
    ASSERT_EQ(timeline.lines.size(), 1);
    EXPECT_EQ(timeline.lines[0].room_id, "new");
}

TEST(MessageTimelineTest, StructuredKindsAndSystemZeroIdClassification) {
    MessageTimeline timeline;
    timeline.add_chat_message("room-uuid", "general", 42, "alice", "hello world", 1700000000000ULL);
    timeline.add_chat_message("", "(system)", 0, "(system)", "system notice", 1700000001000ULL);
    timeline.add_whisper("bob", "alice", "secret", false, 1700000002000ULL);
    timeline.add_whisper("alice", "bob", "reply", true, 1700000003000ULL);
    timeline.add_error("permission denied", 1700000004000ULL);

    ASSERT_EQ(timeline.lines.size(), 5);
    EXPECT_EQ(timeline.lines[0].kind, ChatLineKind::CHAT);
    EXPECT_EQ(timeline.lines[0].room_name, "general");
    EXPECT_EQ(timeline.lines[0].sender, "alice");
    EXPECT_EQ(timeline.lines[0].text, "hello world");
    EXPECT_EQ(timeline.lines[0].timestamp_ms, 1700000000000ULL);

    EXPECT_EQ(timeline.lines[1].kind, ChatLineKind::SYSTEM_NOTICE);
    EXPECT_EQ(timeline.lines[1].message_id, 0);

    EXPECT_EQ(timeline.lines[2].kind, ChatLineKind::WHISPER_IN);
    EXPECT_EQ(timeline.lines[2].sender, "bob");

    EXPECT_EQ(timeline.lines[3].kind, ChatLineKind::WHISPER_OUT);
    EXPECT_EQ(timeline.lines[3].recipient, "bob");

    EXPECT_EQ(timeline.lines[4].kind, ChatLineKind::ERROR_NOTICE);
}

TEST(ClientAppTest, LoginCredentialsAndSecretRedactionAcrossDisconnect) {
    FakeClientTransport transport;
    ClientApp app(transport);

    ASSERT_TRUE(app.connect("127.0.0.1", 6000));
    app.update();
    app.login("alice-display", "chat1.access.secret", false);

    ASSERT_EQ(transport.logins.size(), 1);
    EXPECT_EQ(transport.logins[0].user, "alice-display");
    EXPECT_EQ(transport.logins[0].token, "chat1.access.secret");

    TransportSessionInfo info;
    info.principal_id = "account-001";
    info.auth_session_id = "auth-sess-1";
    info.display_name = "Alice";
    info.logical_session_id = "logical-sess-1";
    info.resume_token = "opaque-resume-secret-xyz";
    info.resume_expires_unix_ms = 1800000000000ULL;
    info.resumed = false;

    transport.on_session_info(info);
    transport.on_login_res("account-001", 77, true);
    app.update();

    const auto& data = app.get_data();
    EXPECT_TRUE(data.is_connected);
    EXPECT_TRUE(data.is_logged_in);
    EXPECT_TRUE(data.is_admin);
    EXPECT_TRUE(data.session.has_resume_token);
    EXPECT_TRUE(app.can_resume());
    EXPECT_EQ(data.session.principal_id, "account-001");
    EXPECT_EQ(data.session.logical_session_id, "logical-sess-1");

    // Secrets must never appear in timeline or public summaries.
    EXPECT_FALSE(timeline_contains(data.timeline, "opaque-resume-secret-xyz"));
    EXPECT_FALSE(timeline_contains(data.timeline, "chat1.access.secret"));

    // Disconnect preserves timeline and resume capability for one-click recovery.
    transport.on_disconnected("eof");
    app.update();
    EXPECT_FALSE(app.get_data().is_connected);
    EXPECT_FALSE(app.get_data().is_logged_in);
    EXPECT_TRUE(app.can_resume());
    EXPECT_FALSE(app.get_data().timeline.lines.empty());

    ASSERT_TRUE(app.reconnect_and_login("127.0.0.1", 6000, "alice-display", "chat1.access.secret", true));
    app.update();
    ASSERT_EQ(transport.logins.size(), 2);
    EXPECT_EQ(transport.logins[1].token, "chat1.access.secret|resume:opaque-resume-secret-xyz");

    // Without access token, resume prefix alone is assembled when requested.
    app.login("alice-display", "", true);
    ASSERT_EQ(transport.logins.size(), 3);
    EXPECT_EQ(transport.logins[2].token, "resume:opaque-resume-secret-xyz");
}

TEST(ClientAppTest, CommandRoutingAndBlacklistSynchronization) {
    FakeClientTransport transport;
    ClientApp app(transport);

    ASSERT_TRUE(app.connect("127.0.0.1", 6000));
    transport.on_login_res("account-001", 10, true);
    transport.on_snapshot(
        "dev-room",
        {"lobby", "dev-room"},
        {"account-001", "bob"},
        {false, true},
        {},
        "account-001",
        10,
        "room-uuid-1",
        true);
    app.update();

    app.process_command("/join secret-room pass123");
    app.process_command("/w bob hello there");
    app.process_command("/refresh");
    app.block_user("bob");
    app.process_command("/mute bob 60");

    ASSERT_EQ(transport.joins.size(), 1);
    EXPECT_EQ(transport.joins[0].room, "secret-room");
    EXPECT_EQ(transport.joins[0].password, "pass123");

    ASSERT_EQ(transport.whispers.size(), 1);
    EXPECT_EQ(transport.whispers[0].user, "bob");
    EXPECT_EQ(transport.whispers[0].text, "hello there");

    ASSERT_EQ(transport.refreshes.size(), 1);
    EXPECT_EQ(transport.refreshes[0], "dev-room");

    ASSERT_EQ(transport.chats.size(), 2);
    EXPECT_EQ(transport.chats[0].room, "dev-room");
    EXPECT_EQ(transport.chats[0].text, "/block bob");
    EXPECT_EQ(transport.chats[1].text, "/mute bob 60");

    // Synchronize blacklist from server system notices.
    transport.on_broadcast("(system)", "(system)", "blacklist: charlie, bob", 0, 0, 0, "", 1700000005000ULL);
    app.update();
    EXPECT_TRUE(app.is_user_blocked("bob"));
    EXPECT_TRUE(app.is_user_blocked("charlie"));

    transport.on_broadcast("(system)", "(system)", "blacklist add: dave", 0, 0, 0, "", 1700000006000ULL);
    transport.on_broadcast("(system)", "(system)", "blacklist denied: cannot target yourself", 0, 0, 0, "", 1700000007000ULL);
    app.update();
    EXPECT_TRUE(app.is_user_blocked("dave"));
    EXPECT_FALSE(app.is_user_blocked("cannot target yourself"));

    transport.on_broadcast("(system)", "(system)", "blacklist remove: bob", 0, 0, 0, "", 1700000008000ULL);
    app.update();
    EXPECT_FALSE(app.is_user_blocked("bob"));
    EXPECT_TRUE(app.is_user_blocked("charlie"));

    transport.on_broadcast("(system)", "(system)", "blacklist: (empty)", 0, 0, 0, "", 1700000009000ULL);
    app.update();
    EXPECT_TRUE(app.get_data().blocked_users.empty());
}

TEST(ClientAppTest, SnapshotRefreshSuppressionPartialHistoryAndPongRtt) {
    FakeClientTransport transport;
    ClientApp app(transport);

    ASSERT_TRUE(app.connect("127.0.0.1", 6000));
    transport.on_login_res("account-001", 11, false);
    transport.on_snapshot(
        "lobby",
        {"lobby"},
        {"account-001"},
        {false},
        {{"alice", "hi", 1700000000100ULL, 1}},
        "account-001",
        1,
        "lobby-uuid",
        true);
    app.update();

    std::size_t joined_notices = 0;
    for (const auto& line : app.get_data().timeline.lines) {
        if (line.kind == ChatLineKind::SYSTEM_NOTICE && line.text == "Joined room: lobby") {
            ++joined_notices;
        }
    }
    EXPECT_EQ(joined_notices, 1u);

    // Refreshing the same room generation does not duplicate the "Joined room" notice.
    transport.on_snapshot(
        "lobby",
        {"lobby"},
        {"account-001", "bob"},
        {false},
        {{"alice", "hi", 1700000000100ULL, 1}, {"bob", "yo", 1700000000200ULL, 2}},
        "account-001",
        2,
        "lobby-uuid",
        false);
    transport.on_pong(14);
    app.update();

    joined_notices = 0;
    for (const auto& line : app.get_data().timeline.lines) {
        if (line.kind == ChatLineKind::SYSTEM_NOTICE && line.text == "Joined room: lobby") {
            ++joined_notices;
        }
    }
    EXPECT_EQ(joined_notices, 1u);
    EXPECT_FALSE(app.get_data().diagnostics.history_available);
    EXPECT_EQ(app.get_data().diagnostics.current_watermark, 2u);
    EXPECT_TRUE(app.get_data().diagnostics.has_rtt);
    EXPECT_EQ(app.get_data().diagnostics.last_rtt_ms, 14u);
}

TEST(MessageTimelineTest, ConsecutiveChatGroupingWindowAndBoundaries) {
    ChatLogLine base;
    base.kind = ChatLineKind::CHAT;
    base.room_id = "room-1";
    base.sender = "alice";
    base.text = "first";
    base.timestamp_ms = 1'700'000'000'000ULL;

    ChatLogLine within_window = base;
    within_window.text = "second";
    within_window.timestamp_ms = base.timestamp_ms + kChatGroupingWindowMs;
    EXPECT_TRUE(can_group_chat_lines(base, within_window));

    ChatLogLine expired = within_window;
    expired.timestamp_ms = base.timestamp_ms + kChatGroupingWindowMs + 1;
    EXPECT_FALSE(can_group_chat_lines(base, expired));

    ChatLogLine other_sender = within_window;
    other_sender.sender = "bob";
    EXPECT_FALSE(can_group_chat_lines(base, other_sender));

    ChatLogLine other_room = within_window;
    other_room.room_id = "room-2";
    EXPECT_FALSE(can_group_chat_lines(base, other_room));

    ChatLogLine reversed_ts = within_window;
    reversed_ts.timestamp_ms = base.timestamp_ms - 1;
    EXPECT_FALSE(can_group_chat_lines(base, reversed_ts));

    ChatLogLine zero_ts = base;
    zero_ts.timestamp_ms = 0;
    EXPECT_FALSE(can_group_chat_lines(zero_ts, within_window));

    ChatLogLine whisper = within_window;
    whisper.kind = ChatLineKind::WHISPER_IN;
    EXPECT_FALSE(can_group_chat_lines(base, whisper));
    EXPECT_FALSE(can_group_chat_lines(whisper, within_window));
}

TEST(MessageTimelineTest, WhisperPeerFilterAndTimelineSearchUtf8) {
    ChatLogLine whisper_in;
    whisper_in.kind = ChatLineKind::WHISPER_IN;
    whisper_in.sender = "bob";
    whisper_in.recipient = "alice";
    whisper_in.text = "Secret ping";

    ChatLogLine whisper_out;
    whisper_out.kind = ChatLineKind::WHISPER_OUT;
    whisper_out.sender = "alice";
    whisper_out.recipient = "bob";
    whisper_out.text = "Secret pong";

    ChatLogLine chat;
    chat.kind = ChatLineKind::CHAT;
    chat.room_name = "dev-lounge";
    chat.sender = "carol";
    chat.text = "\xED\x95\x9C\xEA\xB8\x80 Workbench \xED\x85\x8C\xEC\x8A\xA4\xED\x8A\xB8";

    EXPECT_TRUE(matches_whisper_peer(whisper_in, "bob"));
    EXPECT_TRUE(matches_whisper_peer(whisper_out, "bob"));
    EXPECT_FALSE(matches_whisper_peer(whisper_in, "carol"));
    EXPECT_FALSE(matches_whisper_peer(whisper_in, ""));
    EXPECT_FALSE(matches_whisper_peer(chat, "carol"));

    EXPECT_TRUE(matches_timeline_search(chat, ""));
    EXPECT_TRUE(matches_timeline_search(chat, "bench"));
    EXPECT_TRUE(matches_timeline_search(chat, "\xED\x95\x9C\xEA\xB8\x80"));
    EXPECT_TRUE(matches_timeline_search(chat, "LOUNGE"));
    EXPECT_TRUE(matches_timeline_search(whisper_out, "bob"));
    EXPECT_FALSE(matches_timeline_search(chat, "missing"));
}

TEST(MessageTimelineTest, InputHistoryNavigationDraftAndDedup) {
    InputHistory history(2);
    history.record("");
    EXPECT_EQ(history.size(), 0u);
    EXPECT_EQ(history.navigate_up("draft"), "draft");
    EXPECT_FALSE(history.navigating());

    history.record("/join dev");
    history.record("/join dev");
    EXPECT_EQ(history.size(), 1u);
    history.record("hello");
    history.record("/w bob hi");
    EXPECT_EQ(history.size(), 2u);

    EXPECT_EQ(history.navigate_up("typing..."), "/w bob hi");
    EXPECT_TRUE(history.navigating());
    EXPECT_EQ(history.navigate_up("ignored"), "hello");
    EXPECT_EQ(history.navigate_up("ignored"), "hello");

    EXPECT_EQ(history.navigate_down("ignored"), "/w bob hi");
    EXPECT_EQ(history.navigate_down("ignored"), "typing...");
    EXPECT_FALSE(history.navigating());
}

TEST(ClientAppTest, UnreadWhisperTrackingReadResetAndBlockedSuppression) {
    FakeClientTransport transport;
    ClientApp app(transport);

    ASSERT_TRUE(app.connect("127.0.0.1", 6000));
    transport.on_login_res("alice", 1, false);
    app.update();

    transport.on_whisper("bob", "alice", "hey 1", false);
    transport.on_whisper("alice", "bob", "reply", true);
    transport.on_whisper("carol", "alice", "hey carol", false);
    app.update();

    EXPECT_EQ(app.unread_whisper_count("bob"), 1u);
    EXPECT_EQ(app.unread_whisper_count("carol"), 1u);
    EXPECT_EQ(app.get_data().total_unread_whispers, 2u);

    app.mark_whisper_peer_read("bob");
    EXPECT_EQ(app.unread_whisper_count("bob"), 0u);
    EXPECT_EQ(app.unread_whisper_count("carol"), 1u);
    EXPECT_EQ(app.get_data().total_unread_whispers, 1u);

    app.mark_all_whispers_read();
    EXPECT_EQ(app.unread_whisper_count("carol"), 0u);
    EXPECT_EQ(app.get_data().total_unread_whispers, 0u);

    transport.on_whisper("dave", "alice", "ping 1", false);
    transport.on_whisper("dave", "alice", "ping 2", false);
    app.update();
    EXPECT_EQ(app.unread_whisper_count("dave"), 2u);
    EXPECT_EQ(app.get_data().total_unread_whispers, 2u);

    transport.on_broadcast("(system)", "(system)", "blacklist: dave", 0, 0, 0, "", 1700000005000ULL);
    app.update();
    EXPECT_EQ(app.unread_whisper_count("dave"), 0u);
    EXPECT_EQ(app.get_data().total_unread_whispers, 0u);

    transport.on_whisper("dave", "alice", "still blocked", false);
    app.update();
    EXPECT_EQ(app.unread_whisper_count("dave"), 0u);
    EXPECT_EQ(app.get_data().total_unread_whispers, 0u);
}

TEST(MessageTimelineTest, Utf8WeightedCharUnitsAndCodepointBoundaryTruncation) {
    // 1. Pure ASCII / Latin: 2,000 chars == 2,000 units.
    const std::string ascii_exact(2000, 'a');
    const auto ascii_stats = measure_utf8_char_stats(ascii_exact);
    EXPECT_EQ(ascii_stats.total_codepoints, 2000u);
    EXPECT_EQ(ascii_stats.latin_codepoints, 2000u);
    EXPECT_EQ(ascii_stats.unicode_codepoints, 0u);
    EXPECT_EQ(ascii_stats.weighted_units, 2000u);
    EXPECT_EQ(truncate_utf8_to_char_units(ascii_exact + "extra"), ascii_exact);

    // 2. Latin-1 / Latin Extended letters ('\xC3\xA9' = U+00E9) count as 1 unit; math sign ('\xC3\x97' = U+00D7) counts as 2.
    std::string latin_2000;
    for (int i = 0; i < 2000; ++i) {
        latin_2000 += "\xC3\xA9";
    }
    const auto latin_stats = measure_utf8_char_stats(latin_2000);
    EXPECT_EQ(latin_stats.total_codepoints, 2000u);
    EXPECT_EQ(latin_stats.latin_codepoints, 2000u);
    EXPECT_EQ(latin_stats.unicode_codepoints, 0u);
    EXPECT_EQ(latin_stats.weighted_units, 2000u);
    EXPECT_EQ(truncate_utf8_to_char_units(latin_2000 + "\xC3\xA9").size(), 4000u);

    const auto mixed_sym = measure_utf8_char_stats("a\xC3\x97");
    EXPECT_EQ(mixed_sym.latin_codepoints, 1u);
    EXPECT_EQ(mixed_sym.unicode_codepoints, 1u);
    EXPECT_EQ(mixed_sym.weighted_units, 3u);

    // 3. Korean syllables ('\xEA\xB0\x80' = U+AC00, 3 bytes each): 1,000 chars == 2,000 units (3,000 bytes).
    std::string ko_1000;
    ko_1000.reserve(3000);
    for (int i = 0; i < 1000; ++i) {
        ko_1000 += "\xEA\xB0\x80";
    }
    const auto ko_stats = measure_utf8_char_stats(ko_1000);
    EXPECT_EQ(ko_stats.total_codepoints, 1000u);
    EXPECT_EQ(ko_stats.latin_codepoints, 0u);
    EXPECT_EQ(ko_stats.unicode_codepoints, 1000u);
    EXPECT_EQ(ko_stats.weighted_units, 2000u);
    EXPECT_EQ(truncate_utf8_to_char_units(ko_1000 + "\xEA\xB0\x80"), ko_1000);

    // 4. 4-byte emoji ('\xF0\x9F\x98\x80' = U+1F600): 1,000 chars == 2,000 units (4,000 bytes), never split mid-codepoint.
    std::string emoji_1000;
    emoji_1000.reserve(4000);
    for (int i = 0; i < 1000; ++i) {
        emoji_1000 += "\xF0\x9F\x98\x80";
    }
    const auto emoji_stats = measure_utf8_char_stats(emoji_1000);
    EXPECT_EQ(emoji_stats.total_codepoints, 1000u);
    EXPECT_EQ(emoji_stats.unicode_codepoints, 1000u);
    EXPECT_EQ(emoji_stats.weighted_units, 2000u);
    EXPECT_EQ(truncate_utf8_to_char_units(emoji_1000 + "\xF0\x9F\x98\x80"), emoji_1000);

    // 1,999 Latin chars + 1 emoji (2 units -> total 2,001 > 2,000) must stop cleanly at 1,999 bytes without partial UTF-8.
    const std::string boundary_str = std::string(1999, 'x') + "\xF0\x9F\x98\x80";
    EXPECT_EQ(truncate_utf8_to_char_units(boundary_str).size(), 1999u);
}

TEST(ClientAppTest, OutgoingChatAndWhisperClampToCharUnitLimit) {
    FakeClientTransport transport;
    ClientApp app(transport);

    ASSERT_TRUE(app.connect("127.0.0.1", 6000));
    transport.on_login_res("alice", 1, false);
    app.update();

    std::string ko_1050;
    ko_1050.reserve(1050u * 3u);
    for (int i = 0; i < 1050; ++i) {
        ko_1050 += "\xEA\xB0\x80";
    }

    app.process_command(ko_1050);
    app.whisper("bob", ko_1050);

    ASSERT_EQ(transport.chats.size(), 1u);
    EXPECT_EQ(transport.chats[0].text.size(), 3000u);
    EXPECT_EQ(measure_utf8_char_stats(transport.chats[0].text).unicode_codepoints, 1000u);
    EXPECT_EQ(measure_utf8_char_stats(transport.chats[0].text).weighted_units, kMaxChatCharUnits);

    ASSERT_EQ(transport.whispers.size(), 1u);
    EXPECT_EQ(transport.whispers[0].text.size(), 3000u);
    EXPECT_EQ(measure_utf8_char_stats(transport.whispers[0].text).unicode_codepoints, 1000u);
}

TEST(ClientPreferencesTest, SerializeParseSanitizeAndSecretExclusion) {
    ClientPreferences prefs;
    prefs.host = "chat.example.com\n";
    prefs.port = 7443;
    prefs.username = "alice_dev";
    prefs.workbench_mode = true;
    prefs.text_zoom = 1.25f;
    prefs.hide_blocked_messages = true;
    prefs.auto_resume = true;
    prefs.auto_ping = false;

    const std::string encoded = serialize_client_preferences(prefs);
    EXPECT_EQ(encoded.find("access_token"), std::string::npos);
    EXPECT_EQ(encoded.find("resume_token"), std::string::npos);
    EXPECT_EQ(encoded.find("password"), std::string::npos);

    ClientPreferences parsed;
    const std::string dirty = encoded +
                              "access_token=top-secret-access\n"
                              "resume_token=top-secret-resume\n"
                              "password=room-secret\n"
                              "text_zoom=99.0\n"
                              "port=999999\n";
    parse_client_preferences(dirty, parsed);

    EXPECT_EQ(parsed.host, "chat.example.com");
    EXPECT_EQ(parsed.port, 7443); // invalid 999999 rejected, keeping parsed 7443
    EXPECT_EQ(parsed.username, "alice_dev");
    EXPECT_TRUE(parsed.workbench_mode);
    EXPECT_FLOAT_EQ(parsed.text_zoom, 1.40f); // clamped
    EXPECT_TRUE(parsed.hide_blocked_messages);
    EXPECT_TRUE(parsed.auto_resume);
    EXPECT_FALSE(parsed.auto_ping);

    const auto tmp_dir = std::filesystem::temp_directory_path();
    const auto tmp_file = tmp_dir / "dynaxis_client_prefs_test.cfg";
    std::error_code ec;
    std::filesystem::remove(tmp_file, ec);

    ASSERT_TRUE(save_client_preferences_file(tmp_file, prefs));
    ClientPreferences reloaded;
    ASSERT_TRUE(load_client_preferences_file(tmp_file, reloaded));
    EXPECT_EQ(reloaded.host, "chat.example.com");
    EXPECT_EQ(reloaded.port, 7443);
    EXPECT_EQ(reloaded.username, "alice_dev");
    EXPECT_TRUE(reloaded.workbench_mode);
    EXPECT_TRUE(reloaded.auto_resume);
    EXPECT_FALSE(reloaded.auto_ping);
    std::filesystem::remove(tmp_file, ec);
}

TEST(ClientPreferencesTest, CliArgsParsingAndOverridePrecedence) {
    const char* argv[] = {
        "client_gui",
        "--smoke-test",
        "--host",
        "10.0.0.5",
        "--port=7100",
        "--user",
        "ops_user",
        "--workbench",
        "--auto-resume",
        "--config=custom/client.cfg",
    };
    const ClientCliOptions cli = parse_client_cli_args(10, argv);
    EXPECT_TRUE(cli.smoke_test);
    EXPECT_EQ(cli.host_override, "10.0.0.5");
    EXPECT_EQ(cli.port_override, 7100);
    EXPECT_EQ(cli.user_override, "ops_user");
    EXPECT_EQ(cli.mode_override, CliModeOverride::WORKBENCH);
    EXPECT_EQ(cli.auto_resume_override, 1);
    EXPECT_EQ(cli.config_path_override, std::filesystem::path("custom/client.cfg"));

    ClientPreferences base;
    base.host = "127.0.0.1";
    base.port = 6000;
    base.username = "guest";
    base.workbench_mode = false;
    base.auto_resume = false;
    apply_cli_overrides(base, cli);

    EXPECT_EQ(base.host, "10.0.0.5");
    EXPECT_EQ(base.port, 7100);
    EXPECT_EQ(base.username, "ops_user");
    EXPECT_TRUE(base.workbench_mode);
    EXPECT_TRUE(base.auto_resume);

    EXPECT_EQ(format_remaining_ttl(0, 1000), "-");
    EXPECT_EQ(format_remaining_ttl(1000, 1000), "Expired");
    EXPECT_EQ(format_remaining_ttl(46'000ULL, 1000ULL), "45s");
    EXPECT_EQ(format_remaining_ttl(1000ULL + (14ULL * 60ULL + 5ULL) * 1000ULL, 1000ULL), "14m 05s");
    EXPECT_EQ(format_remaining_ttl(1000ULL + (2ULL * 3600ULL + 3ULL * 60ULL + 4ULL) * 1000ULL, 1000ULL), "2h 03m 04s");
}

TEST(ClientAppTest, AutoResumeExponentialBackoffManualSuppressionAndExpiry) {
    FakeClientTransport transport;
    ClientApp app(transport);
    app.set_auto_resume_enabled(true);

    ASSERT_TRUE(app.connect("127.0.0.1", 6000));
    app.login("alice", "access-secret", false);

    TransportSessionInfo info;
    info.principal_id = "account-001";
    info.resume_token = "resume-secret-abc";
    info.resume_expires_unix_ms = 100'000ULL;
    transport.on_session_info(info);
    transport.on_login_res("account-001", 10, false);
    app.update(500);
    ASSERT_TRUE(app.can_resume());
    EXPECT_EQ(transport.connect_calls, 1);

    // Unexpected disconnect arms attempt #1 with 1,000 ms delay.
    transport.connect_result = false;
    transport.on_disconnected("eof");
    app.update(1'000ULL);
    EXPECT_TRUE(app.get_data().diagnostics.auto_resume_pending);
    EXPECT_EQ(app.get_data().diagnostics.auto_resume_attempt, 1u);
    EXPECT_EQ(app.get_data().diagnostics.auto_resume_next_unix_ms, 2'000ULL);

    app.update(1'999ULL);
    EXPECT_EQ(transport.connect_calls, 1);

    // Failed connect at 2,000 -> attempt #2 after 2,000 ms (at 4,000).
    app.update(2'000ULL);
    EXPECT_EQ(transport.connect_calls, 2);
    EXPECT_EQ(app.get_data().diagnostics.auto_resume_attempt, 2u);
    EXPECT_EQ(app.get_data().diagnostics.auto_resume_next_unix_ms, 4'000ULL);

    // Failed connect at 4,000 -> attempt #3 after 4,000 ms (at 8,000).
    app.update(4'000ULL);
    EXPECT_EQ(transport.connect_calls, 3);
    EXPECT_EQ(app.get_data().diagnostics.auto_resume_attempt, 3u);
    EXPECT_EQ(app.get_data().diagnostics.auto_resume_next_unix_ms, 8'000ULL);

    // Succeed on attempt #3 -> login sent with resume token and counter resets on LoginRes.
    transport.connect_result = true;
    app.update(8'000ULL);
    EXPECT_EQ(transport.connect_calls, 4);
    ASSERT_EQ(transport.logins.size(), 2u);
    EXPECT_EQ(transport.logins[1].token, "access-secret|resume:resume-secret-abc");
    EXPECT_FALSE(app.get_data().diagnostics.auto_resume_pending);

    transport.on_login_res("account-001", 11, false);
    app.update(8'100ULL);
    EXPECT_EQ(app.get_data().diagnostics.auto_resume_attempt, 0u);

    // Manual disconnect must NOT arm Auto-Resume even when enabled.
    app.disconnect();
    EXPECT_FALSE(app.get_data().diagnostics.auto_resume_pending);
    EXPECT_EQ(app.get_data().diagnostics.auto_resume_attempt, 0u);

    // Exhaustion check: 5 failed connect retries stop auto-resume.
    ASSERT_TRUE(app.connect("127.0.0.1", 6000));
    app.update(10'000ULL);
    const int base_connects = transport.connect_calls;
    transport.connect_result = false;
    transport.on_disconnected("drop");
    app.update(10'000ULL); // schedules #1 -> 11,000
    app.update(11'000ULL); // tries #1, schedules #2 -> 13,000
    app.update(13'000ULL); // tries #2, schedules #3 -> 17,000
    app.update(17'000ULL); // tries #3, schedules #4 -> 25,000
    app.update(25'000ULL); // tries #4, schedules #5 -> 41,000
    app.update(41'000ULL); // tries #5, exhausts
    EXPECT_EQ(transport.connect_calls, base_connects + 5);
    EXPECT_FALSE(app.get_data().diagnostics.auto_resume_pending);
    EXPECT_EQ(app.get_data().diagnostics.auto_resume_attempt, 0u);

    // Expired resume token immediately suppresses Auto-Resume.
    transport.connect_result = true;
    ASSERT_TRUE(app.connect("127.0.0.1", 6000));
    transport.on_disconnected("expired-check");
    app.update(100'500ULL);
    EXPECT_FALSE(app.get_data().diagnostics.auto_resume_pending);
}

TEST(ClientAppTest, RttStatsSlidingWindowAndProtocolEventRedaction) {
    FakeClientTransport transport;
    ClientApp app(transport);

    app.set_auto_ping_enabled(false);
    EXPECT_FALSE(app.auto_ping_enabled());
    EXPECT_FALSE(transport.auto_ping_enabled);

    ASSERT_TRUE(app.connect("127.0.0.1", 6000));
    app.login("alice", "my-super-secret-access-token", false);

    TransportSessionInfo info;
    info.principal_id = "account-001";
    info.resume_token = "my-super-secret-resume-token";
    transport.on_session_info(info);
    transport.on_login_res("account-001", 21, true);
    app.update();

    app.join_room("vault", "secret-room-pass");
    transport.on_snapshot("vault", {"lobby", "vault"}, {"account-001"}, {false, true}, {}, "account-001", 9, "vault-uuid", true);
    transport.on_pong(10);
    transport.on_pong(20);
    transport.on_pong(30);
    app.update();

    const auto& diag = app.get_data().diagnostics;
    EXPECT_TRUE(diag.has_rtt);
    EXPECT_EQ(diag.last_rtt_ms, 30u);
    EXPECT_EQ(diag.min_rtt_ms, 10u);
    EXPECT_EQ(diag.max_rtt_ms, 30u);
    EXPECT_EQ(diag.avg_rtt_ms, 20u);
    EXPECT_EQ(diag.rtt_sample_count, 3u);

    bool saw_login_req = false;
    bool saw_join_room = false;
    bool saw_snapshot = false;
    bool saw_pong = false;
    for (const auto& ev : app.get_data().protocol_events) {
        EXPECT_EQ(ev.summary.find("my-super-secret-access-token"), std::string::npos);
        EXPECT_EQ(ev.summary.find("my-super-secret-resume-token"), std::string::npos);
        EXPECT_EQ(ev.summary.find("secret-room-pass"), std::string::npos);
        if (ev.opcode == "MSG_LOGIN_REQ") saw_login_req = true;
        if (ev.opcode == "MSG_JOIN_ROOM") saw_join_room = true;
        if (ev.opcode == "MSG_STATE_SNAPSHOT") saw_snapshot = true;
        if (ev.opcode == "MSG_PONG") saw_pong = true;
    }
    EXPECT_TRUE(saw_login_req);
    EXPECT_TRUE(saw_join_room);
    EXPECT_TRUE(saw_snapshot);
    EXPECT_TRUE(saw_pong);

    app.clear_protocol_events();
    EXPECT_TRUE(app.get_data().protocol_events.empty());
}
