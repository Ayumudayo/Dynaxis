#pragma once

#include <memory>

namespace gateway {

struct GatewayAppAccess;

/**
 * @brief gateway 프로세스의 엣지 유입(edge ingress), 백엔드(backend) 선택, direct-transport 게이트를 조율하는 메인 오케스트레이터입니다.
 *
 * 이 타입은 TCP 리스너로 클라이언트 연결을 수락하고, Redis Instance Registry를 바탕으로
 * 백엔드(`server_app`)를 선택해 TCP 브리지를 구성합니다. 동시에 연결 타임아웃(connect timeout),
 * 재시도 예산(retry budget), circuit breaker, UDP bind abuse guard 같은 보호 장치를 한곳에 모아,
 * 엣지 트래픽 문제와 백엔드 비즈니스 로직을 분리합니다.
 */
class GatewayApp {
public:
    GatewayApp();
    ~GatewayApp();

    /**
     * @brief gateway 메인 루프를 실행합니다.
     * @return 종료 코드(0이면 정상 종료)
     */
    int run();

    /** @brief gateway 전체 종료를 요청합니다. */
    void stop();

private:
    friend struct GatewayAppAccess;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gateway
