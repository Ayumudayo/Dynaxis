#pragma once

#include "admin/admin_types.hpp"
#include "admin/postgres_probe.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace dynaxis::admin {

/**
 * @brief 사전 등록 서비스 카탈로그를 제한된 동시성과 개별 timeout으로 주기적 관측하는 폴러입니다.
 *
 * 한 서비스 노드가 무응답(`hung socket`) 상태여도 제한된 병렬 워커와 `probe_timeout_ms`로 격리해
 * 나머지 서비스와 전체 `/readyz` 판정이 함께 멈추지 않도록 보장합니다.
 */
class InventoryPoller {
public:
    /**
     * @brief 설정, 서비스 카탈로그, 선택적 PostgreSQL probe 구현을 받아 폴러를 초기화합니다.
     * @param config 폴링 주기, timeout, stale TTL, 임계치를 담은 `AdminConfig`
     * @param catalog 관측 대상 `ServiceTargetConfig` 목록
     * @param pg_probe PostgreSQL 상태/마이그레이션 조회 구현체(nullptr이면 config 기반 기본 생성)
     */
    InventoryPoller(AdminConfig config,
                    std::vector<ServiceTargetConfig> catalog,
                    std::shared_ptr<IPostgresProbe> pg_probe = nullptr);

    /** @brief 백그라운드 폴링 스레드를 안전하게 중지하고 조인합니다. */
    ~InventoryPoller();

    /** @brief 백그라운드 주기적 폴링 루프를 시작합니다. */
    void start();

    /** @brief 백그라운드 폴링 루프를 중지하고 종료할 때까지 대기합니다. */
    void stop();

    /**
     * @brief 전체 카탈로그를 즉시 1회 병렬 관측해 최신 `InventorySnapshot`을 갱신·반환합니다.
     * @param now_unix_ms_override 테스트용 결정적 타임스탬프(`0`이면 현재 시스템 시각 사용)
     * @return 최신 `InventorySnapshot` 공유 포인터
     */
    std::shared_ptr<const InventorySnapshot> poll_once(std::uint64_t now_unix_ms_override = 0);

    /**
     * @brief 가장 최근 발행된 `InventorySnapshot`을 락 경합 최소화로 조회합니다.
     * @return 최신 `InventorySnapshot` 공유 포인터 (초기 폴링 전이면 빈 스냅샷)
     */
    std::shared_ptr<const InventorySnapshot> snapshot() const;

    /**
     * @brief 폴러가 첫 번째 관측 주기를 1회 이상 완료했는지 확인합니다.
     * @return 초기 관측 완료 시 true
     */
    bool has_completed_initial_poll() const noexcept;

    /**
     * @brief v3 런타임 설정(`admin.poll_interval_ms`, `admin.stale_ttl_ms`)을 재시작 없이 즉시 반영합니다.
     * @param poll_interval_ms 갱신할 폴링 주기(ms)
     * @param stale_ttl_ms 갱신할 관측 stale 판정 TTL(ms)
     */
    void update_runtime_tuning(std::uint32_t poll_interval_ms, std::uint32_t stale_ttl_ms) noexcept;

    /**
     * @brief 현재 유효한 폴링 주기(ms)를 반환합니다.
     * @return 원자적으로 관리되는 폴링 주기(ms)
     */
    std::uint32_t poll_interval_ms() const noexcept;

    /**
     * @brief 현재 유효한 관측 stale 판정 TTL(ms)을 반환합니다.
     * @return 원자적으로 관리되는 stale TTL(ms)
     */
    std::uint32_t stale_ttl_ms() const noexcept;

    /**
     * @brief 기동 시 구성된 정적 서비스 대상 카탈로그를 반환합니다.
     * @return `ServiceTargetConfig` 목록 상수 참조
     */
    const std::vector<ServiceTargetConfig>& catalog() const noexcept;

private:
    AdminConfig config_;
    std::vector<ServiceTargetConfig> catalog_;
    std::shared_ptr<IPostgresProbe> pg_probe_;
    mutable std::mutex snapshot_mu_;
    std::shared_ptr<const InventorySnapshot> snapshot_;
    std::atomic<std::uint32_t> poll_interval_ms_{5000};
    std::atomic<std::uint32_t> stale_ttl_ms_{15000};
    std::atomic<bool> initial_poll_done_{false};
    std::atomic<bool> stop_requested_{false};
    std::thread worker_thread_;
};

} // namespace dynaxis::admin
