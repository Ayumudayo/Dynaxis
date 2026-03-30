#pragma once

#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <boost/asio/steady_timer.hpp>

#include "gateway/auth/authenticator.hpp"
#include "gateway/transport_session.hpp"
#include "server/core/net/connection.hpp"

namespace gateway {

class GatewayApp;

/**
 * @brief 클라이언트와 backend 서버 사이를 중계하는 gateway 측 연결 클래스입니다.
 *
 * `GatewayConnection`은 클라이언트의 TCP 연결을 관리하고, 인증이 끝나면 `GatewayApp`을
 * 통해 선택된 backend 서버로 브리지를 엽니다. 핵심 목적은 "클라이언트 ingress 처리"와
 * "backend 브리지 처리"를 한 세션 수명주기 안에서 명확히 나누는 것입니다.
 *
 * 주요 역할:
 * 1. 클라이언트 인증(`Authenticator` 위임)
 * 2. backend transport 세션 생성과 수명주기 연결
 * 3. 양방향 payload 포워딩과 direct-path 보조 처리
 */
class GatewayConnection : public server::core::net::TransportConnection {
public:
    GatewayConnection(std::shared_ptr<server::core::net::Hive> hive,
                      std::shared_ptr<auth::IAuthenticator> authenticator,
                      GatewayApp& app);

    void handle_backend_payload(std::vector<std::uint8_t> payload);
    void handle_backend_close(const std::string& reason);

protected:
    void on_connect() override;
    void on_disconnect() override;
    void on_read(const std::uint8_t* data, std::size_t length) override;
    void on_error(const boost::system::error_code& ec) override;

private:
    // 연결은 "로그인 전"과 "브리지 중" 두 단계로만 단순화해 상태 전이를 명확히 유지한다.
    // 이 분리를 두지 않으면 인증 실패, handshake timeout, backend 연결 실패가 한 코드 경로에 섞여
    // 예외 처리와 close 순서가 복잡해진다.
    enum class Phase {
        kWaitingForLogin,
        kBridging,
    };

    // 핸드셰이크 데드라인(초기 로그인 프레임 수신 제한)을 시작한다.
    // 느리거나 비정상적인 클라이언트가 edge 소켓을 오래 점유하는 문제를 줄이기 위한 보호 장치다.
    void start_handshake_deadline();

    // prebuffer_에 누적된 바이트가 "완전한 로그인 프레임"인지 검사한다.
    // 부분 프레임 상태를 명시적으로 다루지 않으면 로그인 직후 예외 경로에서 잘못된 인증 시도가 생길 수 있다.
    bool try_finish_handshake();

    // `GatewayApp`이 선택한 backend(`server_app`)와 TCP 연결을 만든다.
    // 성공 후에는 클라이언트<->backend raw payload를 투명 중계한다.
    void open_backend_connection();

    // 브리지 단계에서 클라이언트 payload를 backend 세션으로 전달한다.
    void send_to_backend(std::vector<std::uint8_t> payload);
    void send_to_backend(const std::uint8_t* data, std::size_t length);
    void inspect_backend_payload(std::span<const std::uint8_t> payload);
    void inspect_login_response_payload(std::span<const std::uint8_t> payload);

    std::shared_ptr<auth::IAuthenticator> authenticator_;
    GatewayApp& app_;
    
    std::string session_id_;
    std::string client_id_;
    std::string resume_routing_key_;
    std::string remote_ip_;
    
    gateway::TransportSessionPtr backend_connection_;
    std::string backend_instance_id_;
    
    auth::AuthResult last_auth_result_{};
    std::atomic<bool> closing_{false};

    Phase phase_{Phase::kWaitingForLogin};
    boost::asio::steady_timer handshake_timer_;
    std::vector<std::uint8_t> prebuffer_;
    std::vector<std::uint8_t> backend_prebuffer_;
};

} // namespace gateway
