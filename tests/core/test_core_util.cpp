#include <gtest/gtest.h>
#include <server/core/util/service_registry.hpp>
#include <string>
#include <memory>

/**
 * @brief ServiceRegistry 등록/조회/타입 안전성 동작을 검증합니다.
 */
using namespace server::core::util::services;

// 테스트용 서비스 구조체
struct TestService {
    std::string name = "TestService";
};

// 또 다른 테스트용 서비스 구조체
struct AnotherService {
    int value = 42;
};

// ServiceRegistry 테스트 픽스처 (각 테스트 전후로 실행될 코드 정의)
class ServiceRegistryTest : public ::testing::Test {
protected:
    // 각 테스트가 끝날 때마다 레지스트리를 비워줌 (테스트 간 간섭 방지)
    void TearDown() override {
        clear();
    }
};

// 서비스 등록 및 조회 테스트
TEST_F(ServiceRegistryTest, SetAndGet) {
    auto service = std::make_shared<TestService>();
    set(service); // 서비스 등록

    auto retrieved = get<TestService>(); // 서비스 조회
    EXPECT_NE(retrieved, nullptr); // 조회된 서비스가 null이 아닌지 확인
    EXPECT_EQ(retrieved->name, "TestService");
    EXPECT_EQ(retrieved, service); // 등록한 객체와 동일한지 확인
}

// emplace를 이용한 서비스 생성 및 등록 테스트
TEST_F(ServiceRegistryTest, Emplace) {
    emplace<AnotherService>(); // 내부에서 생성하여 등록

    auto retrieved = get<AnotherService>();
    EXPECT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->value, 42);
}

// 없는 서비스를 require로 요청했을 때 예외 발생 테스트
TEST_F(ServiceRegistryTest, RequireThrowsOnMissing) {
    // 등록되지 않은 서비스를 require하면 std::runtime_error가 발생해야 함
    EXPECT_THROW(require<TestService>(), std::runtime_error);
}

// 서비스 존재 여부 확인 테스트
TEST_F(ServiceRegistryTest, Has) {
    EXPECT_FALSE(has<TestService>()); // 등록 전에는 false
    set(std::make_shared<TestService>());
    EXPECT_TRUE(has<TestService>());  // 등록 후에는 true
}

// 타입 안전성 테스트
TEST_F(ServiceRegistryTest, TypeSafety) {
    set(std::make_shared<TestService>());
    
    // TestService만 등록했으므로 AnotherService는 없어야 함
    auto retrieved = get<AnotherService>();
    EXPECT_EQ(retrieved, nullptr);
}

TEST_F(ServiceRegistryTest, OwnedEntriesRestorePreviousOwnerWhenCleared) {
    auto left = std::make_shared<TestService>();
    left->name = "left";
    auto right = std::make_shared<TestService>();
    right->name = "right";

    auto& registry = Registry::instance();
    registry.set_owned<TestService>(1, left);
    registry.set_owned<TestService>(2, right);

    ASSERT_TRUE(has<TestService>());
    EXPECT_EQ(require<TestService>().name, "right");

    registry.clear_owned(2);
    ASSERT_TRUE(has<TestService>());
    EXPECT_EQ(require<TestService>().name, "left");

    registry.clear_owned(1);
    EXPECT_FALSE(has<TestService>());
}
