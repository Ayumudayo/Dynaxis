#include <gtest/gtest.h>
#include <server/core/util/paths.hpp>
#include <filesystem>

/**
 * @brief 실행 파일 경로/디렉터리 유틸리티의 기본 계약을 검증합니다.
 */
using namespace server::core::util::paths;

// 실행 파일 경로 관련 테스트
TEST(PathsTest, ExecutablePath) {
    auto path = executable_path();
    
    // 경로가 비어있지 않은지 확인
    EXPECT_FALSE(path.empty());
    
    // 실제로 존재하는 파일인지 확인 (테스트 실행 파일 자신이어야 함)
    EXPECT_TRUE(std::filesystem::exists(path));
}

TEST(PathsTest, ExecutableDir) {
    auto dir = executable_dir();
    
    // 디렉토리가 비어있지 않은지 확인
    EXPECT_FALSE(dir.empty());
    
    // 실제로 존재하는 디렉토리인지 확인
    EXPECT_TRUE(std::filesystem::exists(dir));
    EXPECT_TRUE(std::filesystem::is_directory(dir));
}
