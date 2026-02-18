#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio/steady_timer.hpp>

#include "gateway/auth/authenticator.hpp"
#include "gateway/gateway_app.hpp"
#include "server/core/net/connection.hpp"

namespace gateway {

class GatewayApp;

/**
 * @brief 클라이언트와 백엔드 서버 사이를 중계하는 연결 클래스
 * 
 * GatewayConnection은 클라이언트의 TCP 연결을 관리하며, 수신된 데이터를
 * GatewayApp을 통해 선택된 백엔드 서버로 TCP Forwarding합니다.
 * 
 * 주요 역할:
 * 1. 클라이언트 인증 (Authenticator 위임)
 * 2. 백엔드 서버와의 TCP 연결(BackendConnection) 생성 및 관리
 * 3. 양방향 데이터 포워딩 (Client <-> Server)
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
    // 이 분리를 두지 않으면 인증 실패/타임아웃/백엔드 연결 실패 시 예외 경로가 복잡해진다.
    enum class Phase {
        kWaitingForLogin,
        kBridging,
    };

    // 핸드셰이크 데드라인(초기 로그인 프레임 수신 제한)을 시작한다.
    // 느린/비정상 클라이언트가 소켓을 오래 점유하는 Slowloris류 문제를 줄이기 위한 보호 장치다.
    void start_handshake_deadline();

    // prebuffer_에 누적된 바이트가 "완전한 로그인 프레임"인지 검사하고,
    // 성공 시 인증/백엔드 연결 생성 단계로 진입한다.
    bool try_finish_handshake();

    // GatewayApp이 선택한 backend(server_app)와 TCP 연결을 만든다.
    // 성공 후에는 클라이언트<->백엔드 raw payload를 투명 중계한다.
    void open_backend_connection();

    // 브리지 단계에서 클라이언트 payload를 backend 세션으로 전달한다.
    void send_to_backend(std::vector<std::uint8_t> payload);

    std::shared_ptr<auth::IAuthenticator> authenticator_;
    GatewayApp& app_;
    
    std::string session_id_;
    std::string client_id_;
    std::string remote_ip_;
    
    GatewayApp::BackendConnectionPtr backend_connection_; 
    
    auth::AuthResult last_auth_result_{};
    std::atomic<bool> closing_{false};

    Phase phase_{Phase::kWaitingForLogin};
    boost::asio::steady_timer handshake_timer_;
    std::vector<std::uint8_t> prebuffer_;
};

} // namespace gateway
