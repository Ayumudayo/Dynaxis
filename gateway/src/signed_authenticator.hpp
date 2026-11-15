#pragma once

#include "gateway/auth/authenticator.hpp"
#include "server/auth/session_token.hpp"

namespace gateway::auth {

/** @brief 서명된 신원을 검증합니다. user 입력은 routing identity로 사용하지 않습니다. */
class SignedAuthenticator final : public IAuthenticator {
public:
    /** @brief 앱 조립 시 검증된 signed 설정을 받습니다. */
    explicit SignedAuthenticator(server::auth::Config config) : config_(std::move(config)) {}
    /** @brief 만료/변조/잘못된 credential은 거절하고 토큰은 로그로 출력하지 않습니다. */
    AuthResult authenticate(const AuthRequest& request) override {
        const auto credentials = server::auth::split_credentials(request.token);
        const auto claims = credentials ? server::auth::verify_token(credentials->access_token,
            config_.signing_secret, server::auth::unix_seconds()) : std::nullopt;
        if (!claims) { return {false, {}, "invalid credentials"}; }
        return {true, claims->subject, {}};
    }
private:
    server::auth::Config config_;
};
} // namespace gateway::auth
