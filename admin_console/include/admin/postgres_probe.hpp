#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace dynaxis::admin {

/**
 * @brief PostgreSQL 상태, 활성 방 수, `schema_migrations` 적용 결과 요약입니다.
 */
struct PostgresProbeResult {
    bool healthy{false};
    std::int64_t active_rooms{-1};
    std::int64_t applied_migrations{-1};
    std::int64_t expected_migrations{0};
    bool all_migrations_applied{false};
    std::uint32_t latency_ms{0};
    std::string error;
};

/**
 * @brief PostgreSQL 준비 상태 및 마이그레이션 완료 여부를 조회하는 추상 인터페이스입니다.
 *
 * 실제 실행에서는 `LibpqxxPostgresProbe`를 사용하고, 단위 테스트에서는 가짜(Fake) 구현을
 * 주입해 외부 DB 프로세스 없이 `migrator` 완료/미완료 판정과 비밀 비노출을 검증합니다.
 */
class IPostgresProbe {
public:
    /** @brief 다형성 소멸을 보장하는 기본 가상 소멸자입니다. */
    virtual ~IPostgresProbe() = default;

    /**
     * @brief DB 연결 가능 여부, 활성 방 개수, 마이그레이션 적용 완료 여부를 조회합니다.
     * @param deadline_ms 쿼리 제한 시간(ms)
     * @return 비밀이 제거된 `PostgresProbeResult`
     */
    virtual PostgresProbeResult probe(std::uint32_t deadline_ms) = 0;
};

/**
 * @brief `libpqxx` 기반의 실제 PostgreSQL read-only 상태 조회 구현체입니다.
 */
class LibpqxxPostgresProbe final : public IPostgresProbe {
public:
    /**
     * @brief DB 접속 문자열과 기대 마이그레이션 디렉토리를 받아 probe를 생성합니다.
     * @param db_uri PostgreSQL 연결 URI (`error` 문자열에 절대 노출하지 않음)
     * @param migrations_dir `(\d+)_.*\.sql$` 마이그레이션 파일 디렉토리
     */
    LibpqxxPostgresProbe(std::string db_uri, std::filesystem::path migrations_dir);

    /**
     * @brief `statement_timeout`을 걸고 `rooms` 및 `schema_migrations`를 조회합니다.
     * @param deadline_ms 연결·쿼리 제한 시간(ms)
     * @return 조회 결과 `PostgresProbeResult`
     */
    PostgresProbeResult probe(std::uint32_t deadline_ms) override;

private:
    std::string db_uri_;
    std::filesystem::path migrations_dir_;
};

/**
 * @brief 지정 디렉토리에 존재하는 `(\d+)_.*\.sql$` 마이그레이션 스크립트 수를 셉니다.
 * @param migrations_dir 검사할 마이그레이션 디렉토리 경로
 * @return 유효한 SQL 마이그레이션 파일 개수
 */
std::int64_t count_expected_migrations(const std::filesystem::path& migrations_dir);

/**
 * @brief DB/Redis 예외 메시지에서 URI 자격증명(`password`, `://user:pass@`)을 제거합니다.
 * @param raw 원본 오류 문자열
 * @return 자격증명이 마스킹된 안전한 문자열
 */
std::string sanitize_connection_error(std::string raw);

} // namespace dynaxis::admin
