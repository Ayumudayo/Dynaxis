#pragma once

#include <memory>
#include <functional>
#include <boost/asio.hpp>

namespace server::core {

namespace asio = boost::asio;

class Session;
class Dispatcher;
class BufferManager;
struct SessionOptions;
namespace net {
struct ConnectionRuntimeState;
}

/**
 * @brief 신규 TCP 연결을 수락해 `Session` 객체로 넘기는 수락기입니다.
 *
 * 왜 별도 클래스로 두는가?
 * - 연결 수락/재시도 정책을 세션 처리 로직과 분리하면,
 *   장애 상황(accept 실패, 포트 이슈)에서 복구 로직을 독립적으로 관리하기 쉽습니다.
 * - 최대 연결 상한, accept retry backoff 같은 문지기 정책을 session 내부에 섞지 않게 됩니다.
 */
class Acceptor : public std::enable_shared_from_this<Acceptor> {
public:
    using new_session_cb_t = std::function<void(std::shared_ptr<Session>)>;

    /**
     * @brief 수락기를 생성합니다.
     * @param io 비동기 이벤트 루프
     * @param ep 바인딩할 로컬 endpoint
     * @param dispatcher 패킷 디스패처
     * @param buffer_manager 버퍼 풀 관리자
     * @param options 세션 옵션
     * @param state 연결 수/세션 ID를 추적하는 런타임 상태
     * @param on_new_session 신규 세션 콜백
     */
    Acceptor(asio::io_context& io,
             const asio::ip::tcp::endpoint& ep,
             Dispatcher& dispatcher,
             BufferManager& buffer_manager,
             std::shared_ptr<const SessionOptions> options,
             std::shared_ptr<net::ConnectionRuntimeState> state,
             new_session_cb_t on_new_session = {});

    /** @brief accept 루프를 시작합니다. */
    void start();
    /** @brief accept 루프를 중지합니다. */
    void stop();

private:
    void schedule_accept_retry();
    void do_accept();

    asio::io_context& io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::steady_timer accept_retry_timer_;
    std::atomic<bool> running_{false};
    Dispatcher& dispatcher_;
    BufferManager& buffer_manager_;
    std::shared_ptr<const SessionOptions> options_;
    std::shared_ptr<net::ConnectionRuntimeState> state_;
    new_session_cb_t on_new_session_{};
};

} // namespace server::core

namespace server::core::net {

using Acceptor = ::server::core::Acceptor;
using SessionListener = ::server::core::Acceptor;

} // namespace server::core::net
