// UTF-8, 한국어 주석
#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>

#include "client/client_transport.hpp"
#include "server/core/protocol/packet.hpp"

/**
 * @brief Boost.Asio 기반 TCP 네트워크 클라이언트 구현체입니다.
 *
 * `IClientTransport`를 구현해 Gateway/HAProxy와의 비동기 TCP 연결,
 * 14-byte 프레이밍, Protobuf 직렬화/역직렬화 및 `MSG_PING`/`MSG_PONG` RTT 측정을 처리합니다.
 * UI와 비즈니스 로직(`ClientApp`)은 등록된 콜백을 통해서만 이벤트를 전달받습니다.
 */
class NetClient : public IClientTransport {
public:
    /** @brief `IClientTransport`의 세션 정보 구조체 별칭입니다. */
    using SessionInfo = TransportSessionInfo;
    /** @brief `IClientTransport`의 스냅샷 메시지 구조체 별칭입니다. */
    using SnapshotMessage = ::SnapshotMessage;

    /** @brief 네트워크 송수신 컨텍스트와 타이머 상태를 초기화합니다. */
    NetClient();
    /** @brief 활성 연결과 백그라운드 I/O 스레드를 안전하게 종료합니다. */
    ~NetClient() override;

    /**
     * @brief `MSG_HELLO` 수신 콜백을 등록합니다.
     * @param f 서버 capability 비트를 받을 콜백
     */
    void set_on_hello(OnHello f) override { on_hello_ = std::move(f); }

    /**
     * @brief `MSG_ERR` 수신 콜백을 등록합니다.
     * @param f 오류 코드와 메시지를 받을 콜백
     */
    void set_on_err(OnErr f) override { on_err_ = std::move(f); }

    /**
     * @brief 로그인 응답 콜백을 등록합니다.
     * @param f 확정 사용자명, 세션 ID, 관리자 여부를 받을 콜백
     */
    void set_on_login_res(OnLoginRes f) override { on_login_ = std::move(f); }

    /**
     * @brief 로그인 응답의 인증·연속성 상태 콜백을 등록합니다.
     * @param f 인증·연속성 상세 정보를 받을 콜백
     */
    void set_on_session_info(OnSessionInfo f) override { on_session_info_ = std::move(f); }

    /**
     * @brief 채팅/시스템 브로드캐스트 콜백을 등록합니다.
     * @param f 브로드캐스트 필드를 받을 콜백
     */
    void set_on_broadcast(OnBroadcast f) override { on_bcast_ = std::move(f); }

    /**
     * @brief 방 참여자 목록 콜백을 등록합니다.
     * @param f 방 이름과 참여자 목록을 받을 콜백
     */
    void set_on_room_users(OnRoomUsers f) override { on_room_users_ = std::move(f); }

    /**
     * @brief 상태 스냅샷 콜백을 등록합니다.
     * @param f 스냅샷 데이터를 받을 콜백
     */
    void set_on_snapshot(OnSnapshot f) override { on_snapshot_ = std::move(f); }

    /**
     * @brief 귓속말 브로드캐스트 콜백을 등록합니다.
     * @param f 귓속말 송수신 내용을 받을 콜백
     */
    void set_on_whisper(OnWhisper f) override { on_whisper_ = std::move(f); }

    /**
     * @brief 귓속말 결과 콜백을 등록합니다.
     * @param f 귓속말 성공 여부와 사유를 받을 콜백
     */
    void set_on_whisper_result(OnWhisperResult f) override { on_whisper_result_ = std::move(f); }

    /**
     * @brief 상태 갱신 알림 콜백을 등록합니다.
     * @param f `MSG_REFRESH_NOTIFY` 수신 시 호출할 콜백
     */
    void set_on_refresh_notify(OnRefreshNotify f) override { on_refresh_notify_ = std::move(f); }

    /**
     * @brief 연결 종료 콜백을 등록합니다.
     * @param f 종료 사유를 받을 콜백
     */
    void set_on_disconnected(OnDisconnected f) override { on_disconnected_ = std::move(f); }

    /**
     * @brief 연결 상태 전이 콜백을 등록합니다.
     * @param f 변경된 `TransportState`를 받을 콜백
     */
    void set_on_state_changed(OnStateChanged f) override { on_state_changed_ = std::move(f); }

    /**
     * @brief `MSG_PONG` RTT 측정 콜백을 등록합니다.
     * @param f 왕복 지연(ms)을 받을 콜백
     */
    void set_on_pong(OnPong f) override { on_pong_ = std::move(f); }

    /**
     * @brief 서버에 TCP 연결을 수립하고 비동기 수신·ping 루프를 시작합니다.
     * @param host 서버 주소 (IP 또는 도메인)
     * @param port 서버 포트
     * @return 연결 수립 및 I/O 스레드 시작에 성공하면 true
     */
    bool connect(const std::string& host, unsigned short port) override;

    /**
     * @brief TCP 연결과 백그라운드 I/O 스레드를 정리하고 종료합니다.
     */
    void close() override;

    /**
     * @brief 현재 연결되어 있는지 확인합니다.
     * @return 활성 TCP 연결 중이면 true
     */
    bool is_connected() const override { return connected_.load(); }

    /**
     * @brief 로그인 요청(`MSG_LOGIN_REQ`)을 전송합니다.
     * @param user 요청 닉네임 또는 식별자
     * @param token access 또는 `access|resume:...` 자격 문자열
     */
    void send_login(const std::string& user, const std::string& token) override;

    /**
     * @brief 방 입장 또는 생성 요청(`MSG_JOIN_ROOM`)을 전송합니다.
     * @param room 방 이름
     * @param password 비밀번호(선택)
     */
    void send_join(const std::string& room, const std::string& password = std::string()) override;

    /**
     * @brief 방 퇴장 요청(`MSG_LEAVE_ROOM`)을 전송합니다.
     * @param room 퇴장할 방 이름
     */
    void send_leave(const std::string& room = std::string()) override;

    /**
     * @brief 채팅 또는 서버 슬래시 명령(`MSG_CHAT_SEND`)을 전송합니다.
     * @param room 현재 방 이름
     * @param text 본문 또는 슬래시 명령
     */
    void send_chat(const std::string& room, const std::string& text) override;

    /**
     * @brief 현재 방 스냅샷 재요청(`MSG_REFRESH_REQ`)을 전송합니다.
     * @param current_room 현재 방 이름
     */
    void send_refresh(const std::string& current_room) override;

    /**
     * @brief 특정 방 사용자 목록 요청(`MSG_ROOM_USERS_REQ`)을 전송합니다.
     * @param room 대상 방 이름
     */
    void send_who(const std::string& room) override;

    /**
     * @brief 방 목록 요청(`MSG_ROOMS_REQ`)을 전송합니다.
     * @param current_room 현재 방 이름
     */
    void send_rooms(const std::string& current_room) override;

    /**
     * @brief 귓속말 전송 요청(`MSG_WHISPER_REQ`)을 전송합니다.
     * @param user 대상 사용자 식별자
     * @param text 귓속말 본문
     */
    void send_whisper(const std::string& user, const std::string& text) override;

    /**
     * @brief keepalive 및 RTT 측정을 위한 `MSG_PING`을 전송합니다.
     */
    void send_ping() override;

    /**
     * @brief 주기적 자동 `MSG_PING` 하트비트 송신 활성화 여부를 설정합니다.
     * @param enabled 10초 주기 자동 `MSG_PING`을 활성화하면 true
     */
    void set_auto_ping_enabled(bool enabled) override;

private:
    using Tcp = boost::asio::ip::tcp;
    using Strand = boost::asio::strand<boost::asio::io_context::executor_type>;
    using Timer = boost::asio::steady_timer;
    using WorkGuard = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;

    void start_read_header();
    void start_read_body(const server::core::protocol::PacketHeader& header);
    void handle_packet(const server::core::protocol::PacketHeader& header, std::span<const std::uint8_t> body);
    void enqueue_packet(std::uint16_t msg_id, std::uint16_t flags, std::vector<std::uint8_t> payload = {});
    void enqueue_packet_on_strand(std::uint16_t msg_id, std::uint16_t flags, std::vector<std::uint8_t> payload = {});
    void send_ping_on_strand();
    void drain_send_queue();
    void schedule_ping();
    void handle_disconnect(const boost::system::error_code& ec, const char* context);

    boost::asio::io_context io_;
    Tcp::resolver resolver_{io_};
    Tcp::socket socket_{io_};
    Strand strand_{io_.get_executor()};
    std::deque<std::shared_ptr<std::vector<std::uint8_t>>> write_queue_;
    std::unique_ptr<Timer> ping_timer_;
    std::unique_ptr<WorkGuard> work_guard_;
    std::thread io_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> auto_ping_enabled_{true};
    std::vector<std::uint8_t> read_header_;
    std::vector<std::uint8_t> read_body_;
    std::uint32_t seq_{1};
    std::chrono::steady_clock::time_point last_ping_sent_{};
    bool ping_in_flight_{false};

    OnHello on_hello_;
    OnErr on_err_;
    OnLoginRes on_login_;
    OnSessionInfo on_session_info_;
    OnBroadcast on_bcast_;
    OnRoomUsers on_room_users_;
    OnSnapshot on_snapshot_;
    OnWhisper on_whisper_;
    OnWhisperResult on_whisper_result_;
    OnRefreshNotify on_refresh_notify_;
    OnDisconnected on_disconnected_;
    OnStateChanged on_state_changed_;
    OnPong on_pong_;
};
