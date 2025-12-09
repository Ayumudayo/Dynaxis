#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

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
 * 2. 백엔드 서버와의 TCP 세션(BackendSession) 생성 및 관리
 * 3. 양방향 데이터 포워딩 (Client <-> Server)
 */
class GatewayConnection : public server::core::net::Connection {
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
    void open_backend_session();
    void send_to_backend(const std::vector<std::uint8_t>& payload);

    std::shared_ptr<auth::IAuthenticator> authenticator_;
    GatewayApp& app_;
    
    std::string session_id_; 
    std::string client_id_;  
    
    GatewayApp::BackendSessionPtr backend_session_; 
    
    bool authenticated_{false};
    auth::AuthResult last_auth_result_{};
    std::atomic<bool> closing_{false};
};

} // namespace gateway
