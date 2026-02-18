#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>

#include "server/core/app/termination_signals.hpp"
#include "server/core/metrics/http_server.hpp"

namespace server::core::app {

/**
 * @brief 서비스 프로세스 공통 런타임 제어를 모아 둔 경량 호스트입니다.
 *
 * 왜 필요한가?
 * - `server_app`, `gateway_app`, `wb_worker`는 역할은 다르지만
 *   "종료 신호 처리 / readiness / dependency 상태" 규칙은 동일해야 운영이 단순해집니다.
 * - 공통 계층이 없으면 각 바이너리가 shutdown/health 로직을 제각각 구현하게 되어,
 *   장애 시 동작 차이(예: 특정 프로세스만 ready를 늦게 내리는 문제)로 분석 비용이 커집니다.
 *
 * 책임 범위(의도적으로 작게 유지):
 * - 프로세스 생명주기 플래그(stop/healthy/ready) 관리
 * - `METRICS_PORT` 기반 admin HTTP(`/metrics`, `/healthz`, `/readyz`) 기동
 * - SIGINT/SIGTERM 기반 graceful shutdown 훅 실행
 */
class AppHost {
public:
    enum class DependencyRequirement : std::uint8_t {
        kRequired,
        kOptional,
    };

    explicit AppHost(std::string name);
    ~AppHost();

    AppHost(const AppHost&) = delete;
    AppHost& operator=(const AppHost&) = delete;

    /**
     * @brief 정지 요청 플래그를 올립니다.
     * @return 이번 호출에서 처음으로 정지 요청이 들어갔으면 true, 이미 요청된 상태였으면 false
     */
    bool request_stop() noexcept;

    /**
     * @brief 정지 요청 여부를 조회합니다.
     * @return 정지 요청이 들어왔으면 true
     */
    bool stop_requested() const noexcept;

    /**
     * @brief `/healthz` 기준의 프로세스 건강 상태를 설정합니다.
     * @param healthy true면 healthy, false면 unhealthy
     */
    void set_healthy(bool healthy) noexcept;

    /**
     * @brief `/healthz` 기준의 건강 상태를 조회합니다.
     * @return healthy 상태면 true
     */
    bool healthy() const noexcept;

    /**
     * @brief 기본 readiness 플래그를 설정합니다.
     * @param ready true면 기본 준비 완료, false면 미준비
     */
    void set_ready(bool ready) noexcept;

    /**
     * @brief 현재 readiness 결과를 조회합니다.
     * @return 기본 readiness와 의존성 상태를 모두 만족하면 true
     */
    bool ready() const noexcept;

    /**
     * @brief readiness 판정에 포함할 의존성을 등록합니다.
     * @param name 의존성 이름(예: redis, postgres)
     * @param requirement 필수/선택 의존성 여부
     *
     * 필수 의존성은 `set_dependency_ok()` 호출 전까지 기본적으로 미준비(false)로 취급됩니다.
     */
    void declare_dependency(std::string name,
                            DependencyRequirement requirement = DependencyRequirement::kRequired);

    /**
     * @brief 의존성 상태를 갱신합니다.
     * @param name 의존성 이름
     * @param ok 현재 의존성 준비 상태
     *
     * 사전 등록되지 않은 이름이 들어오면 운영 실수를 조기에 드러내기 위해 경고를 남기고,
     * Required 의존성으로 간주하여 즉시 레지스트리에 편입합니다.
     */
    void set_dependency_ok(std::string_view name, bool ok);

    /**
     * @brief 필수 의존성이 모두 준비되었는지 조회합니다.
     * @return 필수 의존성 전체가 ready면 true
     */
    bool dependencies_ok() const noexcept;

    /**
     * @brief `/healthz` 응답 본문을 생성합니다.
     * @param ok 현재 healthz 판정 결과
     * @return 상태 원인을 포함한 텍스트 응답 본문
     */
    std::string health_body(bool ok) const;

    /**
     * @brief `/readyz` 응답 본문을 생성합니다.
     * @param ok 현재 readyz 판정 결과
     * @return 상태 원인을 포함한 텍스트 응답 본문
     */
    std::string readiness_body(bool ok) const;

    /**
     * @brief 의존성 상태를 Prometheus 텍스트 포맷으로 직렬화합니다.
     * @return dependency 메트릭 텍스트
     */
    std::string dependency_metrics_text() const;

    /**
     * @brief admin HTTP 서버를 시작합니다.
     * @param port 수신 포트
     * @param metrics_callback `/metrics` 본문 생성 콜백
     */
    void start_admin_http(unsigned short port,
                          server::core::metrics::MetricsHttpServer::MetricsCallback metrics_callback);

    /**
     * @brief admin HTTP 서버를 중지합니다.
     */
    void stop_admin_http();

    /**
     * @brief 종료 시 실행할 shutdown 단계를 등록합니다.
     * @param name 단계 식별 이름(로그/관측 용도)
     * @param step 실행할 정리 함수
     *
     * 등록 역순(LIFO) 실행으로 의존성 해제 순서를 제어하기 쉽습니다.
     * (예: acceptor stop -> background thread stop -> storage close)
     */
    void add_shutdown_step(std::string name, std::function<void()> step);

    /**
     * @brief 지정된 `io_context`에 비동기 종료 시그널 핸들러를 설치합니다.
     * @param io 시그널을 감시할 Asio 이벤트 루프
     * @param on_shutdown 실제 자원 정리를 수행할 콜백
     *
     * `on_shutdown`은 리스너/워커 중지 같은 정리를 수행하고,
     * 최종적으로 이벤트 루프가 종료되도록 유도해야 합니다.
     */
    void install_asio_termination_signals(boost::asio::io_context& io,
                                          std::function<void()> on_shutdown);

private:
    struct DependencyRegistry;
    struct ShutdownRegistry;

    void run_shutdown_steps() noexcept;

    std::string name_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> healthy_{true};
    std::atomic<bool> startup_ready_{false};
    std::atomic<bool> deps_ok_{true};

    std::atomic<bool> shutdown_ran_{false};

    std::unique_ptr<DependencyRegistry> deps_;
    std::unique_ptr<ShutdownRegistry> shutdown_;

    std::unique_ptr<server::core::metrics::MetricsHttpServer> admin_http_;
    std::unique_ptr<boost::asio::signal_set> signals_;
};

} // namespace server::core::app
