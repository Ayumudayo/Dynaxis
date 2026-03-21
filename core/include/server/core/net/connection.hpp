#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include <boost/asio.hpp>

#include "server/core/net/hive.hpp"

namespace server::core::net {

/**
 * @brief 공용 TCP transport substrate를 제공하는 연결 클래스입니다.
 *
 * `Connection`은 packet semantics를 모르는 generic transport 계층입니다. 이 분리가
 * 중요한 이유는, gateway나 다른 consumer가 동일한 read/write/backpressure 골격을
 * 재사용하되 현재 서버의 `Session` 의미까지 끌어안지 않게 하기 위해서입니다.
 *
 * 주요 역할:
 * 1. TCP 소켓 생명주기 관리
 * 2. 비동기 읽기/쓰기 루프 유지
 * 3. 송신 큐를 통한 직렬 전송과 bounded backpressure 제공
 *
 * 사용 시에는 보통 상속으로 `on_read`, `on_connect`, `on_error` 등을 오버라이드합니다.
 */
class Connection : public std::enable_shared_from_this<Connection> {
public:
    using socket_type = boost::asio::ip::tcp::socket;
    static constexpr std::size_t k_default_send_queue_max = 256 * 1024;

    /**
     * @brief 연결 객체를 생성합니다.
     * @param hive I/O 컨텍스트 수명주기를 관리하는 `Hive`
     * @param send_queue_max_bytes 송신 큐 총 바이트 상한
     */
    explicit Connection(std::shared_ptr<Hive> hive,
                        std::size_t send_queue_max_bytes = k_default_send_queue_max);
    virtual ~Connection();

    // 연결 객체는 소켓/큐 상태를 직접 소유하므로 복사를 금지합니다.
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    /**
     * @brief 내부 TCP 소켓을 반환합니다.
     * @return 연결이 소유한 TCP 소켓 참조
     */
    socket_type& socket();

    /**
     * @brief 연결을 시작하고 비동기 읽기 루프를 엽니다.
     *
     * 소켓이 이미 연결되었거나 accept를 통해 준비된 뒤 호출해야 합니다.
     */
    void start();

    /**
     * @brief 연결을 종료합니다.
     *
     * 이후 소켓/큐/콜백은 정리 경로로 들어가며 더 이상 입출력을 지속하지 않습니다.
     */
    void stop();

    /**
     * @brief 연결이 종료되었는지 확인합니다.
     * @return 연결이 정지 상태면 `true`
     */
    bool is_stopped() const;

    /**
     * @brief 데이터를 비동기 송신 큐에 넣습니다.
     *
     * 데이터는 즉시 쓰기되지 않을 수 있으며, 내부 쓰기 큐에 저장된 뒤 strand를 통해
     * 순차적으로 전송됩니다. 이 구조 덕분에 여러 호출 지점이 동시에 전송을 요청해도
     * 쓰기 순서와 큐 상한을 한곳에서 관리할 수 있습니다.
     *
     * @param data 전송할 바이트 배열
     */
    void async_send(std::vector<std::uint8_t> data);

protected:
    // ======================================================================
    // 하위 클래스에서 구현해야 할 가상 함수들 (이벤트 핸들러)
    // ======================================================================

    /**
     * @brief 연결이 성공적으로 시작되었을 때 호출됩니다.
     */
    virtual void on_connect();

    /**
     * @brief 연결이 끊어졌을 때 호출됩니다.
     */
    virtual void on_disconnect();

    /**
     * @brief 데이터를 수신했을 때 호출됩니다.
     * @param data 수신된 데이터의 포인터
     * @param length 수신된 데이터의 길이 (바이트)
     */
    virtual void on_read(const std::uint8_t* data, std::size_t length);

    /**
     * @brief 데이터 전송이 완료되었을 때 호출됩니다.
     * @param length 전송된 데이터의 길이 (바이트)
     */
    virtual void on_write(std::size_t length);

    /**
     * @brief 에러가 발생했을 때 호출됩니다.
     * @param ec Boost.Asio 에러 코드
     */
    virtual void on_error(const boost::system::error_code& ec);

    /**
     * @brief I/O 컨텍스트를 반환합니다.
     * 타이머 설정 등 추가적인 비동기 작업에 필요할 수 있습니다.
     */
    boost::asio::io_context& io();

private:
    /**
     * @brief 지속적으로 데이터를 읽는 루프입니다.
     * 비동기 읽기 요청을 발행하고, 완료되면 handle_read를 호출합니다.
     */
    void read_loop();

    /**
     * @brief 비동기 읽기 완료 콜백
     */
    void handle_read(const boost::system::error_code& ec, std::size_t bytes_transferred);

    /**
     * @brief 비동기 쓰기 완료 콜백
     */
    void handle_write(const boost::system::error_code& ec,
                      std::size_t bytes_transferred,
                      std::size_t packet_size);

    /**
     * @brief 쓰기 큐에 있는 데이터를 실제로 전송합니다.
     * 현재 전송 중인 데이터가 없을 때만 호출됩니다.
     */
    void do_write();
    void finalize_stop();

    std::shared_ptr<Hive> hive_;
    socket_type socket_;
    boost::asio::strand<boost::asio::any_io_executor> strand_;
    
    // 수신 버퍼: 한 번에 최대 4KB까지 읽습니다.
    std::array<std::uint8_t, 4096> read_buffer_{};
    
    // 송신 큐: 전송 대기 중인 패킷들을 저장합니다.
    std::deque<std::shared_ptr<std::vector<std::uint8_t>>> write_queue_;
    std::size_t queued_bytes_{0};
    std::size_t send_queue_max_bytes_{k_default_send_queue_max};
    
    std::atomic<bool> stopped_{false};
};

using TransportConnection = Connection;

} // namespace server::core::net
