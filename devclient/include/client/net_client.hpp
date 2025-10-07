// UTF-8, 한국어 주석
#pragma once
#include <functional>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <boost/asio.hpp>

class NetClient {
public:
    using OnHello = std::function<void(std::uint16_t caps)>;
    using OnErr = std::function<void(std::uint16_t code, std::string msg)>;
    using OnLoginRes = std::function<void(std::string effective_user, std::uint32_t sid)>;
    using OnBroadcast = std::function<void(std::string room, std::string sender, std::string text, std::uint16_t flags, std::uint32_t sender_sid)>;
    using OnRoomUsers = std::function<void(std::string room, std::vector<std::string> users)>;
    using OnSnapshot = std::function<void(std::string current, std::vector<std::string> rooms, std::vector<std::string> users, std::vector<bool> locked)>;
    using OnWhisper = std::function<void(std::string sender, std::string recipient, std::string text, bool outgoing)>;
    using OnWhisperResult = std::function<void(bool ok, std::string reason)>;

    NetClient();
    ~NetClient();

    void set_on_hello(OnHello f) { on_hello_ = std::move(f); }
    void set_on_err(OnErr f) { on_err_ = std::move(f); }
    void set_on_login_res(OnLoginRes f) { on_login_ = std::move(f); }
    void set_on_broadcast(OnBroadcast f) { on_bcast_ = std::move(f); }
    void set_on_room_users(OnRoomUsers f) { on_room_users_ = std::move(f); }
    void set_on_snapshot(OnSnapshot f) { on_snapshot_ = std::move(f); }
    void set_on_whisper(OnWhisper f) { on_whisper_ = std::move(f); }
    void set_on_whisper_result(OnWhisperResult f) { on_whisper_result_ = std::move(f); }

    bool connect(const std::string& host, unsigned short port);
    void close();

    void send_login(const std::string& user, const std::string& token);
    void send_join(const std::string& room, const std::string& password = std::string());
    void send_leave(const std::string& room = std::string());
    void send_chat(const std::string& room, const std::string& text);
    void send_refresh(const std::string& current_room);
    void send_who(const std::string& room);
    void send_rooms(const std::string& current_room);
    void send_whisper(const std::string& user, const std::string& text);

private:
    void recv_loop();
    void start_threads();
    static void send_frame_simple(boost::asio::ip::tcp::socket& sock, std::uint16_t msg_id, std::uint16_t flags, const std::vector<std::uint8_t>& payload, std::uint32_t& tx_seq, std::mutex* send_mu = nullptr);

    boost::asio::io_context io_;
    boost::asio::ip::tcp::resolver resolver_{io_};
    boost::asio::ip::tcp::socket sock_{io_};
    std::thread rx_thread_;
    std::thread ping_thread_;
    std::mutex send_mu_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::uint32_t seq_{1};

    OnHello on_hello_;
    OnErr on_err_;
    OnLoginRes on_login_;
    OnBroadcast on_bcast_;
    OnRoomUsers on_room_users_;
    OnSnapshot on_snapshot_;
    OnWhisper on_whisper_;
    OnWhisperResult on_whisper_result_;
};

