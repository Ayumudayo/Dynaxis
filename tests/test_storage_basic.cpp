// UTF-8, 한국어 주석
#include <gtest/gtest.h>
#include <cstdlib>
#include <string>
#include <chrono>

#include "server/storage/postgres/connection_pool.hpp"
#include "server/core/storage/connection_pool.hpp"
#include "server/core/storage/repositories.hpp"

/**
 * @brief Postgres 저장소 기본 시나리오(방/메시지/멤버십) 통합 검증 테스트입니다.
 */
using namespace server::core::storage;

namespace {

std::string unique_name(const std::string& prefix) {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return prefix + std::to_string(ms);
}

}

// 방 생성, 유저 입장, 메시지 전송, 멤버십 조회 등 기본적인 스토리지 시나리오를 검증합니다.
// 실제 DB 연결이 필요하므로 DB_URI가 없으면 건너뜁니다.
TEST(StorageBasic, RoomMessageMembershipHappyPath) {
    const char* db = std::getenv("DB_URI");
    if (!db || !*db) {
        GTEST_SKIP() << "DB_URI 미설정 — 테스트 건너뜀";
    }
    PoolOptions popts{};
    auto pool = server::storage::postgres::make_connection_pool(db, popts);
    ASSERT_TRUE(pool);
    if (!pool->health_check()) {
        GTEST_SKIP() << "DB health_check 실패 — 테스트 건너뜀";
    }

    // 1) 방 생성/조회
    auto room_name = unique_name("gtest-room-");
    std::string room_id;
    {
        auto uow = pool->make_unit_of_work();
        auto created = uow->rooms().create(room_name, true);
        room_id = created.id;
        uow->commit();
    }
    {
        auto uow = pool->make_unit_of_work();
        auto found = uow->rooms().find_by_name_exact_ci(room_name);
        ASSERT_TRUE(found.has_value());
        EXPECT_EQ(found->name, room_name);
    }

    // 2) 유저 생성 및 메시지 작성
    std::string user_id;
    std::uint64_t msg_id = 0;
    {
        auto uow = pool->make_unit_of_work();
        auto user = uow->users().create_guest(unique_name("guest-"));
        user_id = user.id;
        auto msg = uow->messages().create(room_id, room_name, user_id, "hello from gtest");
        msg_id = msg.id;
        ASSERT_GT(msg_id, 0u);
        uow->commit();
    }
    {
        auto uow = pool->make_unit_of_work();
        auto last = uow->messages().get_last_id(room_id);
        EXPECT_GE(last, msg_id);
        auto recent = uow->messages().fetch_recent_by_room(room_id, 0, 10);
        ASSERT_FALSE(recent.empty());
        bool seen = false; for (auto& m : recent) if (m.id == msg_id) { seen = true; break; }
        EXPECT_TRUE(seen);
    }

    // 3) 멤버십 upsert + last_seen 갱신/조회
    {
        auto uow = pool->make_unit_of_work();
        uow->memberships().upsert_join(user_id, room_id, "member");
        uow->memberships().update_last_seen(user_id, room_id, msg_id);
        uow->commit();
    }
    {
        auto uow = pool->make_unit_of_work();
        auto last_seen = uow->memberships().get_last_seen(user_id, room_id);
        ASSERT_TRUE(last_seen.has_value());
        EXPECT_EQ(*last_seen, msg_id);
    }
}
