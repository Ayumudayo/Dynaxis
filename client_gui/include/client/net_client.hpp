// UTF-8, 한국어 주석
#pragma once
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <span>

#include <boost/asio.hpp>

#include "server/core/protocol/packet.hpp"

/**
 * @brief 네트워크 클라이언트 클래스
 * 
 * 서버(Gateway)와의 TCP 연결을 관리하고, 비동기로 메시지를 송수신합니다.
 * UI 로직과 분리하기 위해 콜백(Callback) 패턴을 사용합니다.
 * 
 * 주요 기능:
 * - 비동기 연결 및 재연결 관리
 * - 패킷 직렬화/역직렬화 (Protobuf + PacketHeader)
 * - 수신된 메시지를 UI 레이어로 전달 (콜백 호출)
 * - 백그라운드 스레드에서 I/O 처리
 */
class NetClient {
public:
    // ======================================================================
    // 콜백 함수 타입 정의
    // 서버로부터 메시지를 받았을 때 호출될 함수들의 시그니처입니다.
    // ======================================================================
    
    using OnHello = std::function<void(std::uint16_t caps)>;
    using OnErr = std::function<void(std::uint16_t code, std::string msg)>;
    using OnLoginRes = std::function<void(std::string effective_user, std::uint32_t sid, bool is_admin)>;
    using OnBroadcast = std::function<void(std::string room, std::string sender, std::string text, std::uint16_t flags, std::uint32_t sender_sid)>;
    using OnRoomUsers = std::function<void(std::string room, std::vector<std::string> users)>;
    /** @brief 스냅샷 메시지의 송신자/본문/시각 필드입니다. */
    struct SnapshotMessage {
        std::string sender;
        std::string text;
        std::uint64_t ts_ms;
    };
    using OnSnapshot = std::function<void(std::string current, std::vector<std::string> rooms, std::vector<std::string> users, std::vector<bool> locked, std::vector<SnapshotMessage> messages, std::string your_name)>;
    using OnWhisper = std::function<void(std::string sender, std::string recipient, std::string text, bool outgoing)>;
    using OnWhisperResult = std::function<void(bool ok, std::string reason)>;
    using OnRefreshNotify = std::function<void()>;
    using OnDisconnected = std::function<void(std::string reason)>;

    NetClient();
    ~NetClient();

    // --- 콜백 등록 메서드 ---
    void set_on_hello(OnHello f) { on_hello_ = std::move(f); }
    void set_on_err(OnErr f) { on_err_ = std::move(f); }
    void set_on_login_res(OnLoginRes f) { on_login_ = std::move(f); }
    void set_on_broadcast(OnBroadcast f) { on_bcast_ = std::move(f); }
    void set_on_room_users(OnRoomUsers f) { on_room_users_ = std::move(f); }
    void set_on_snapshot(OnSnapshot f) { on_snapshot_ = std::move(f); }
    void set_on_whisper(OnWhisper f) { on_whisper_ = std::move(f); }
    void set_on_whisper_result(OnWhisperResult f) { on_whisper_result_ = std::move(f); }
    void set_on_refresh_notify(OnRefreshNotify f) { on_refresh_notify_ = std::move(f); }
    void set_on_disconnected(OnDisconnected f) { on_disconnected_ = std::move(f); }

    /**
     * @brief 서버에 연결합니다.
     * @param host 서버 주소 (IP 또는 도메인)
     * @param port 서버 포트
     * @return 연결 시도 성공 여부 (즉시 반환, 실제 연결은 비동기)
     */
    bool connect(const std::string& host, unsigned short port);
    
    /**
     * @brief 연결을 종료합니다.
     */
    void close();
    
    /**
     * @brief 현재 연결되어 있는지 확인합니다.
     */
    bool is_connected() const { return connected_.load(); }

    // --- 메시지 전송 메서드 ---
    
    void send_login(const std::string& user, const std::string& token);
    void send_join(const std::string& room, const std::string& password = std::string());
    void send_leave(const std::string& room = std::string());
    void send_chat(const std::string& room, const std::string& text);
    void send_refresh(const std::string& current_room);
    void send_who(const std::string& room);
    void send_rooms(const std::string& current_room);
    void send_whisper(const std::string& user, const std::string& text);

private:
    using Tcp = boost::asio::ip::tcp;
    using Strand = boost::asio::strand<boost::asio::io_context::executor_type>;
    using Timer = boost::asio::steady_timer;
    using WorkGuard = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;

    void start_read_header();
    void start_read_body(const server::core::protocol::PacketHeader& header);
    void handle_packet(const server::core::protocol::PacketHeader& header, std::span<const std::uint8_t> body);
    void enqueue_packet(std::uint16_t msg_id, std::uint16_t flags, std::vector<std::uint8_t> payload = {});
    void drain_send_queue();
    void schedule_ping();
    void handle_disconnect(const boost::system::error_code& ec, const char* context);
    void reset_state();

    boost::asio::io_context io_;
    Tcp::resolver resolver_{io_};
    Tcp::socket socket_{io_};
    Strand strand_{io_.get_executor()}; // 동기화 처리를 위한 Strand
    std::deque<std::shared_ptr<std::vector<std::uint8_t>>> write_queue_; // 송신 큐
    std::unique_ptr<Timer> ping_timer_;
    std::unique_ptr<WorkGuard> work_guard_;
    std::thread io_thread_; // I/O 작업을 처리하는 백그라운드 스레드
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::vector<std::uint8_t> read_header_;
    std::vector<std::uint8_t> read_body_;
    std::uint32_t seq_{1};

    // 등록된 콜백 함수들
    OnHello on_hello_;
    OnErr on_err_;
    OnLoginRes on_login_;
    OnBroadcast on_bcast_;
    OnRoomUsers on_room_users_;
    OnSnapshot on_snapshot_;
    OnWhisper on_whisper_;
    OnWhisperResult on_whisper_result_;
    OnRefreshNotify on_refresh_notify_;
    OnDisconnected on_disconnected_;
};

