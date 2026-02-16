#pragma once

namespace server::app {

/**
 * @brief `server_app` 부트스트랩을 실행합니다.
 * @param argc 커맨드라인 인자 개수
 * @param argv 커맨드라인 인자 배열
 * @return 프로세스 종료 코드(0이면 정상 종료)
 */
int run_server(int argc, char** argv);

} // namespace server::app
