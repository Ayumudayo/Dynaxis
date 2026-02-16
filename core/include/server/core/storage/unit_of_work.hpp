#pragma once

#include <memory>

#include "server/core/storage/repositories.hpp"

namespace server::core::storage {

/**
 * @brief 트랜잭션 단위를 나타내는 인터페이스입니다.
 *
 * 저장소 계층은 `IUnitOfWork` 경계를 기준으로 commit/rollback 책임을 분리해,
 * 실패 복구 시점과 범위를 명확하게 유지합니다.
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

    /** @brief 사용자 리포지터리 접근자를 반환합니다. */
    virtual IUserRepository& users() = 0;
    /** @brief 방 리포지터리 접근자를 반환합니다. */
    virtual IRoomRepository& rooms() = 0;
    /** @brief 메시지 리포지터리 접근자를 반환합니다. */
    virtual IMessageRepository& messages() = 0;
    /** @brief 세션 리포지터리 접근자를 반환합니다. */
    virtual ISessionRepository& sessions() = 0;
    /** @brief 멤버십 리포지터리 접근자를 반환합니다. */
    virtual IMembershipRepository& memberships() = 0;
};

} // namespace server::core::storage
