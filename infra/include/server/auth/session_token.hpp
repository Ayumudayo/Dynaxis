#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace server::auth {

/** @brief 앱 인증 정책입니다. 코어가 아닌 gateway/server 조립 계층에서 설정합니다. */
struct Config {
    bool required{false};
    std::string signing_secret;
};

/** @brief 서명으로 검증한 신원입니다. subject는 불변 식별자이고 display_name은 표시용입니다. */
struct SessionClaims {
    std::string subject;
    std::string session_id;
    std::string display_name;
    std::uint64_t issued_at{};
    std::uint64_t expires_at{};
};

/** @brief access credential과 채팅 continuity credential은 서로 대체할 수 없습니다. */
struct Credentials {
    std::string access_token;
    std::string resume_token;
};

/** @brief 기본 signed 모드는 32-byte 이상 secret이 없으면 시작을 거절합니다. development는 명시적 호환 모드입니다. */
Config load_config_from_env();
std::uint64_t unix_seconds();
/** @brief access token과 선택적 |resume: suffix를 분리합니다. 잘못되거나 과대한 입력은 거절합니다. */
std::optional<Credentials> split_credentials(std::string_view token);
/** @brief 서버가 신뢰하는 issuer에서만 사용합니다. 잘못된 claims/짧은 key는 예외입니다. */
std::string issue_token(const SessionClaims& claims, std::string_view secret);
/** @brief 서명·issuer·audience·버전·시간·타입·길이를 검증하고 실패 시 null을 반환합니다. */
std::optional<SessionClaims> verify_token(std::string_view token, std::string_view secret, std::uint64_t now);

} // namespace server::auth
