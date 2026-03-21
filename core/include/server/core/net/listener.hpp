#pragma once

#include <atomic>
#include <functional>
#include <memory>

#include <boost/asio.hpp>

#include "server/core/net/connection.hpp"
#include "server/core/net/hive.hpp"

namespace server::core::net {

/**
 * @brief TCP 리스너의 공통 수락 루프를 제공하는 베이스 클래스입니다.
 *
 * `connection_factory`를 주입받아 어떤 `Connection` 파생 객체를 만들지 외부에서 결정합니다.
 * 덕분에 gateway/server가 동일한 수락 골격을 공유하면서도 서로 다른 연결 타입을 사용할 수 있습니다.
 *
 * 중요한 점은 이 타입이 accept loop까지만 소유한다는 것입니다. 연결 이후의 packet/session
 * 의미까지 리스너가 알게 하면, 공용 transport 계층이 앱별 도메인 의미에 오염됩니다.
 */
class Listener : public std::enable_shared_from_this<Listener> {
public:
    using connection_factory = std::function<std::shared_ptr<Connection>(std::shared_ptr<Hive>)>;

    /**
     * @brief 리스너를 생성합니다.
     * @param hive 이벤트 루프 수명주기 소유자
     * @param endpoint 바인딩할 endpoint
     * @param factory 수락 시 사용할 연결 객체 생성기
     */
    Listener(std::shared_ptr<Hive> hive,
             const boost::asio::ip::tcp::endpoint& endpoint,
             connection_factory factory);
    virtual ~Listener();

    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    /** @brief accept 루프를 시작합니다. */
    void start();
    /** @brief accept 루프를 중지합니다. */
    void stop();
    /**
     * @brief 리스너 정지 여부를 반환합니다.
     * @return 리스너가 정지 상태면 `true`
     */
    bool is_stopped() const;
    /**
     * @brief 실제 바인딩된 로컬 endpoint를 반환합니다.
     * @return 바인딩된 로컬 TCP endpoint
     */
    boost::asio::ip::tcp::endpoint local_endpoint() const;

protected:
    /** @brief 연결 수락 성공 시 호출되는 확장 포인트입니다. 수락 이후의 앱별 초기화는 여기서 이어집니다. */
    virtual void on_accept(std::shared_ptr<Connection> connection);
    /** @brief 수락/소켓 계층 오류 발생 시 호출되는 확장 포인트입니다. */
    virtual void on_error(const boost::system::error_code& ec);

private:
    void do_accept();

    std::shared_ptr<Hive> hive_;
    boost::asio::ip::tcp::acceptor acceptor_;
    connection_factory factory_;
    std::atomic<bool> stopped_{false};
};

using TransportListener = Listener;

} // namespace server::core::net
