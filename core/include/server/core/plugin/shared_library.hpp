#pragma once

#include <filesystem>
#include <string>

namespace server::core::plugin {

/**
 * @brief 플랫폼별 동적 라이브러리 로딩을 감싸는 RAII 래퍼입니다.
 */
class SharedLibrary {
public:
    SharedLibrary() = default;
    ~SharedLibrary();

    SharedLibrary(const SharedLibrary&) = delete;
    SharedLibrary& operator=(const SharedLibrary&) = delete;

    SharedLibrary(SharedLibrary&& other) noexcept;
    SharedLibrary& operator=(SharedLibrary&& other) noexcept;

    /**
     * @brief 지정 경로의 라이브러리를 로드합니다.
     * @param path 라이브러리 파일 경로
     * @param error 실패 시 오류 메시지
     * @return 로드 성공 여부
     */
    bool open(const std::filesystem::path& path, std::string& error);

    /** @brief 로드된 라이브러리를 언로드합니다. */
    void close();

    /**
     * @brief 라이브러리에서 심볼을 조회합니다.
     * @param name 심볼 이름
     * @param error 실패 시 오류 메시지
     * @return 심볼 주소 (실패 시 nullptr)
     */
    void* symbol(const char* name, std::string& error) const;

    /**
     * @brief 라이브러리 로드 상태를 반환합니다.
     * @return 로드되어 있으면 true
     */
    bool is_loaded() const;

private:
    void* handle_{nullptr};
};

} // namespace server::core::plugin
