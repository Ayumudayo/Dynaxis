#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "server/core/storage/unit_of_work.hpp"

namespace server::core::storage {

/**
 * @brief DB 연결 풀 동작 파라미터입니다.
 */
struct PoolOptions {
    std::size_t min_size{1};               ///< 풀 최소 연결 수
    std::size_t max_size{10};              ///< 풀 최대 연결 수
    std::uint32_t connect_timeout_ms{5000};///< 연결 타임아웃(ms)
    std::uint32_t query_timeout_ms{5000};  ///< 쿼리 타임아웃(ms)
    bool prepare_statements{true};         ///< prepare statement 사용 여부
};

/**
 * @brief 데이터베이스 연결 생명주기와 generic transaction 경계를 관리하는 SPI 인터페이스입니다.
 */
class IConnectionPool {
public:
    virtual ~IConnectionPool() = default;

    /**
     * @brief generic transaction 단위 객체를 생성합니다.
     * @return 생성된 UnitOfWork
     *
     * UnitOfWork 소멸 시점에 연결 반환과 미완료 트랜잭션 롤백 책임이 구현체에 있습니다.
     */
    virtual std::unique_ptr<IUnitOfWork> make_unit_of_work() = 0;

    /**
     * @brief 연결 풀의 건강 상태를 점검합니다.
     * @return 점검 성공 시 true
     */
    virtual bool health_check() = 0;
};

} // namespace server::core::storage
