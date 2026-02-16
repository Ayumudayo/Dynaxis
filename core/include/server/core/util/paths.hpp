#pragma once

#include <filesystem>

namespace server::core::util::paths {

/**
 * @brief 현재 실행 파일의 절대 경로를 반환합니다.
 * @return 실행 파일 절대 경로
 */
std::filesystem::path executable_path();

/**
 * @brief 현재 실행 파일이 위치한 디렉터리 경로를 반환합니다.
 * @return 실행 파일 디렉터리 절대 경로
 */
std::filesystem::path executable_dir();

} // namespace server::core::util::paths
