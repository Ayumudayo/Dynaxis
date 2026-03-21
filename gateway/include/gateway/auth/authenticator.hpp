#pragma once

#include <string>

namespace gateway::auth {

/**
 * @brief 인증 요청 입력값을 담는 구조체입니다.
 *
 * gateway는 인증 자체를 구현체에 위임하되, 어떤 정보가 인증 경계로 넘어가는지는 이 계약으로 고정합니다.
 */
struct AuthRequest {
    std::string token;          ///< 클라이언트가 보낸 인증 토큰(예: JWT, 세션키)
    std::string client_id;      ///< 클라이언트가 주장하는 식별자(예: username)
    std::string remote_address; ///< 클라이언트 원격 IP 주소
};

/**
 * @brief 인증 결과 구조체입니다.
 *
 * `subject`와 `failure_reason`를 함께 두는 이유는, edge에서 성공/실패 후속 동작과 운영 로그를
 * 같은 결과 객체 위에서 처리하기 위해서입니다.
 */
struct AuthResult {
    bool success{false};          ///< 인증 성공 여부
    std::string subject;          ///< 인증된 주체 ID
    std::string failure_reason;   ///< 실패 사유 텍스트
};

/**
 * @brief 인증 로직을 추상화한 인터페이스입니다.
 *
 * 실제 구현체는 JWT 검증, DB 조회, 외부 인증 API 호출 등을 수행할 수 있습니다.
 * gateway가 인증 세부를 모르도록 이 경계를 유지해야, 인증 공급자 교체가 edge 브리지 로직까지 흔들지 않습니다.
 */
class IAuthenticator {
public:
    virtual ~IAuthenticator() = default;

    /**
     * @brief 인증 요청을 검증합니다.
     * @param request 인증 입력값
     * @return 인증 결과
     */
    virtual AuthResult authenticate(const AuthRequest& request) = 0;
};

/**
 * @brief 인증을 우회하는 개발용 기본 구현체입니다.
 *
 * 개발/로컬 smoke에서 ingress 경로 자체를 먼저 점검할 때 유용하지만, 운영 인증 대체재로 쓰면 안 됩니다.
 */
class NoopAuthenticator final : public IAuthenticator {
public:
    /**
     * @brief 요청을 항상 성공 처리합니다.
     * @param request 인증 요청
     * @return success=true 인증 결과
     */
    AuthResult authenticate(const AuthRequest& request) override {
        AuthResult result;
        result.success = true;
        result.subject = request.client_id.empty() ? "anonymous" : request.client_id;
        return result;
    }
};

} // namespace gateway::auth
