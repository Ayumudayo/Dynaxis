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
 * @brief TCP 연결을 관리하는 클래스입니다.
 * 
 * Connection 클래스는 클라이언트와의 1:1 TCP 연결을 추상화합니다.
 * Boost.Asio의 비동기 I/O 기능을 사용하여 데이터를 주고받습니다.
 * 
 * 주요 역할:
 * 1. 소켓(Socket) 관리: TCP 소켓의 생명주기를 관리합니다.
 * 2. 비동기 읽기/쓰기: 블로킹 없이 데이터를 송수신합니다.
 * 3. 버퍼링: 수신된 데이터를 버퍼에 저장하고, 송신할 데이터를 큐에 쌓아 순차적으로 보냅니다.
 * 
 * 사용법:
 * - 이 클래스는 상속받아 사용해야 합니다. (on_read, on_connect 등을 오버라이드)
 * - `std::enable_shared_from_this`를 상속받아 비동기 작업 중 객체가 파괴되지 않도록 보장합니다.
 */
class Connection : public std::enable_shared_from_this<Connection> {
public:
    using socket_type = boost::asio::ip::tcp::socket;
    static constexpr std::size_t k_default_send_queue_max = 256 * 1024;

    /**
     * @brief 생성자
     * @param hive I/O 컨텍스트를 관리하는 Hive 객체 (스레드 풀과 연결됨)
     */
    explicit Connection(std::shared_ptr<Hive> hive,
                        std::size_t send_queue_max_bytes = k_default_send_queue_max);
    virtual ~Connection();

    // 복사 방지: 연결 객체는 고유해야 하므로 복사를 금지합니다.
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    /**
     * @brief 내부 소켓 객체를 반환합니다.
     * 주로 연결 수락(Accept) 시 소켓을 초기화하기 위해 사용됩니다.
     */
    socket_type& socket();

    /**
     * @brief 연결을 시작합니다.
     * 소켓이 연결된 후 호출해야 하며, 비동기 읽기 작업을 시작합니다.
     */
    void start();

    /**
     * @brief 연결을 종료합니다.
     * 소켓을 닫고 더 이상 입출력을 수행하지 않습니다.
     */
    void stop();

    /**
     * @brief 연결이 종료되었는지 확인합니다.
     */
    bool is_stopped() const;

    /**
     * @brief 데이터를 비동기로 전송합니다.
     * 
     * 데이터는 즉시 전송되지 않을 수 있으며, 내부 쓰기 큐(write_queue_)에 저장된 후
     * 순차적으로 전송됩니다. 이는 여러 스레드에서 동시에 전송을 요청해도 안전하게 처리하기 위함입니다.
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
    void handle_write(const boost::system::error_code& ec, std::size_t bytes_transferred);

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
    std::deque<std::vector<std::uint8_t>> write_queue_;
    std::size_t queued_bytes_{0};
    std::size_t send_queue_max_bytes_{k_default_send_queue_max};
    
    std::atomic<bool> stopped_{false};
};

} // namespace server::core::net
