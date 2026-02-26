#include <exception>

#include "gateway/gateway_app.hpp"
#include "server/core/util/log.hpp"

/**
 * @brief `gateway_app` 프로세스 진입점입니다.
 *
 * 초기화/런타임 예외를 최상위에서 포착해 로그를 남기고,
 * 운영 스크립트가 판별 가능한 종료 코드를 반환합니다.
 */
int main() {
    try {
        // GatewayApp 인스턴스를 생성하고 메인 루프를 시작합니다.
        // 예외가 발생하면 로그를 남기고 종료합니다.
        gateway::GatewayApp app;
        return app.run();
    } catch (const std::exception& ex) {
        server::core::log::error(std::string("GatewayApp fatal error: ") + ex.what());
    } catch (...) {
        server::core::log::error("GatewayApp fatal error: unknown exception");
    }
    return 1;
}
