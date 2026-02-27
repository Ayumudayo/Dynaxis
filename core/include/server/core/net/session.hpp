#pragma once

#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <queue>
#include <string>
#include <vector>

#include "server/core/memory/memory_pool.hpp"
#include "server/core/protocol/opcode_policy.hpp"
#include "server/core/protocol/packet.hpp"

namespace server::core {

namespace asio = boost::asio;

class Dispatcher;
class BufferManager;
struct SessionOptions;
namespace net {
struct ConnectionRuntimeState;
}

using PacketHeader = server::core::protocol::PacketHeader;

/**
 * @brief 클라이언트 1개 TCP 연결의 수명주기와 패킷 송수신을 담당합니다.
 *
 * 왜 필요한가?
 * - 읽기/쓰기/heartbeat/timeout을 세션 단위로 캡슐화해 연결 격리를 보장합니다.
 * - 장애가 특정 세션에서 발생해도 다른 세션과 서버 전체 이벤트 루프는 계속 동작합니다.
 */
class Session : public std::enable_shared_from_this<Session> {
public:
    /**
     * @brief 세션 객체를 생성합니다.
     * @param socket 수락된 TCP 소켓
     * @param dispatcher opcode 라우팅 디스패처
     * @param buffer_manager 송수신 버퍼 풀 관리자
     * @param options 세션 제어 옵션
     * @param state 연결 수/세션 ID를 추적하는 런타임 상태
     */
    Session(asio::ip::tcp::socket socket,
            Dispatcher& dispatcher,
            BufferManager& buffer_manager,
            std::shared_ptr<const SessionOptions> options,
            std::shared_ptr<net::ConnectionRuntimeState> state);

    /** @brief 세션 읽기/타이머 루프를 시작합니다. */
    void start();
    /** @brief 세션을 종료하고 소켓/타이머를 정리합니다. */
    void stop();

    /**
     * @brief 세션 strand에서 콜백을 직렬 실행으로 예약합니다.
     * @param fn 실행할 콜백
     * @return 예약 성공 시 true, 세션이 shared ownership 상태가 아니면 false
     */
    bool post_serialized(std::function<void()> fn);

    /**
     * @brief 세션 종료 직전에 1회 호출할 콜백을 등록합니다.
     * @param cb 종료 콜백
     */
    void set_on_close(std::function<void(std::shared_ptr<Session>)> cb) { on_close_ = std::move(cb); }

    /**
     * @brief `MSG_ERR` 패킷을 만들어 전송합니다.
     * @param code 오류 코드
     * @param msg 오류 메시지 텍스트
     */
    void send_error(std::uint16_t code, const std::string& msg);

    /**
     * @brief 이미 직렬화된 패킷 버퍼를 전송 큐에 추가합니다.
     * @param data 전송할 직렬화 버퍼
     * @param packet_size 버퍼 내 실제 패킷 바이트 수
     */
    void async_send(BufferManager::PooledBuffer data, size_t packet_size);

    /**
     * @brief msg_id + payload를 패킷으로 직렬화해 전송 큐에 추가합니다.
     * @param msg_id 메시지 ID(opcode)
     * @param payload 패킷 payload
     * @param flags 프로토콜 플래그
     */
    void async_send(std::uint16_t msg_id, const std::vector<std::uint8_t>& payload, std::uint16_t flags = 0);

    /**
     * @brief 세션 ID를 반환합니다.
     * @return 서버에서 할당한 세션 ID
     */
    std::uint32_t session_id() const { return session_id_; }

    /**
     * @brief 세션 인증/권한 상태를 갱신합니다.
     * @param status 새 세션 상태
     */
    void set_session_status(server::core::protocol::SessionStatus status) noexcept {
        session_status_.store(status, std::memory_order_release);
    }

    /**
     * @brief 현재 세션 인증/권한 상태를 반환합니다.
     * @return 현재 세션 상태
     */
    server::core::protocol::SessionStatus session_status() const noexcept {
        return session_status_.load(std::memory_order_acquire);
    }

private:
    void do_read_header();
    void do_read_body(std::size_t body_len);
    void do_write();
    std::pair<BufferManager::PooledBuffer, size_t> make_packet(std::uint16_t msg_id,
                                                              std::uint16_t flags,
                                                              const std::vector<std::uint8_t>& payload,
                                                              std::uint32_t seq,
                                                              std::uint32_t utc_ts_ms32);
    void send_hello();
    void arm_read_timeout();
    void arm_write_timeout();
    void arm_heartbeat();

public:
    /**
     * @brief 클라이언트 원격 IP를 문자열로 반환합니다.
     * @return 조회 실패 시 빈 문자열
     */
    std::string remote_ip() const;

private:

    asio::ip::tcp::socket socket_;
    asio::strand<asio::any_io_executor> strand_;
    Dispatcher& dispatcher_;
    BufferManager& buffer_manager_;
    PacketHeader header_{};
    BufferManager::PooledBuffer read_buf_;
    std::queue<std::pair<BufferManager::PooledBuffer, size_t>> send_queue_;
    std::size_t queued_bytes_{0};
    std::shared_ptr<const SessionOptions> options_;
    std::shared_ptr<net::ConnectionRuntimeState> state_;
    std::atomic<bool> stopped_{false};
    boost::asio::steady_timer read_timer_{socket_.get_executor()};
    boost::asio::steady_timer write_timer_{socket_.get_executor()};
    boost::asio::steady_timer heartbeat_timer_{socket_.get_executor()};
    std::uint32_t tx_seq_{1};
    std::function<void(std::shared_ptr<Session>)> on_close_{};
    std::uint32_t session_id_{0};
    std::atomic<server::core::protocol::SessionStatus> session_status_{
        server::core::protocol::SessionStatus::kAny};
};

} // namespace server::core

namespace server::core::net {

using Session = ::server::core::Session;
using PacketSession = ::server::core::Session;

} // namespace server::core::net

