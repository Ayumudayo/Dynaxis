// 진입점은 모든 초기화를 server::app::run_server로 위임한다.
#include "server/app/bootstrap.hpp"

/**
 * @brief `server_app` 프로세스 진입점입니다.
 *
 * 부트스트랩 로직을 단일 함수로 위임해,
 * 초기화 경로와 종료 코드를 일관되게 관리합니다.
 */
int main(int argc, char** argv) {
    return server::app::run_server(argc, argv);
}
