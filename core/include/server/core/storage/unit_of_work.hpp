#pragma once

namespace server::core::storage {

/**
 * @brief 트랜잭션 단위를 나타내는 인터페이스입니다.
 *
 * 저장소 계층은 `IUnitOfWork` 경계를 기준으로 commit/rollback 책임을 분리해,
 * 실패 복구 시점과 범위를 명확하게 유지합니다.
 * 이 코어 seam은 도메인 리포지터리 접근자를 소유하지 않고, 오직 트랜잭션 경계만 정의합니다.
 */
class IUnitOfWork {
public:
    virtual ~IUnitOfWork() = default;

    /**
     * @brief 트랜잭션 작업을 확정합니다.
     *
     * 실패 시 예외를 던지며, 호출자는 rollback을 시도해야 합니다.
     */
    virtual void commit() = 0;

    /**
     * @brief 트랜잭션을 취소합니다.
     *
     * commit 이전에는 여러 번 호출되어도 무방해야 합니다(idempotent).
     */
    virtual void rollback() = 0;

};

} // namespace server::core::storage
