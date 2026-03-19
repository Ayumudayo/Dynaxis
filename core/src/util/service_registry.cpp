#include "server/core/util/service_registry.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>

/**
 * @brief 경량 DI 서비스 레지스트리 구현입니다.
 *
 * 실행 중 모듈 경계를 넘어 동일 서비스 인스턴스를 공유해야 할 때,
 * 타입 기반 키와 환경 변수 포인터 공유를 조합해 참조 일관성을 유지합니다.
 */
namespace server::core::util::services {

namespace {
// DLL이나 테스트 바이너리가 분리된 주소 공간에서 동일 레지스트리를 재사용하기 위한 환경 변수 키
constexpr const char* kRegistryEnvVar = "SERVER_CORE_SERVICE_REGISTRY";

std::string pointer_to_string(const Registry* reg) {
    std::ostringstream oss;
    oss << reg;
    return oss.str();
}

Registry* string_to_pointer(const char* value) {
    if (!value || !*value) {
        return nullptr;
    }
    std::istringstream iss(value);
    void* ptr = nullptr;
    iss >> ptr;
    return static_cast<Registry*>(ptr);
}
} // namespace

Registry& Registry::instance() {
    static Registry singleton;

    auto existing = std::getenv(kRegistryEnvVar);
    if (existing) {
        if (auto shared_ptr = string_to_pointer(existing); shared_ptr) {
            return *shared_ptr;
        }
    }

    // 환경 변수로 레지스트리 주소를 공유하면 모듈 경계를 넘어 같은 인스턴스를 재사용할 수 있다.
    // 이는 DLL이나 Shared Object로 분리된 플러그인들이 메인 프로세스의 서비스를 참조해야 할 때 유용하다.
    auto pointer_str = pointer_to_string(&singleton);
#if defined(_WIN32)
    _putenv_s(kRegistryEnvVar, pointer_str.c_str());
#else
    setenv(kRegistryEnvVar, pointer_str.c_str(), 1);
#endif
    return singleton;
}

void Registry::set_any(std::string key, std::any value, OwnerToken owner) {
    // Registry는 경량 DI 용도로 사용되므로 std::any로 원시 포인터도 안전하게 저장한다.
    // owner token을 함께 보관해 runtime별 compatibility bridge 정리를 분리한다.
    std::lock_guard<std::mutex> lock(mutex_);
    auto& entries = services_[key];
    entries.erase(
        std::remove_if(
            entries.begin(),
            entries.end(),
            [owner](const Entry& entry) { return entry.owner_token == owner; }),
        entries.end());
    entries.push_back(Entry{
        .value = std::move(value),
        .owner_token = owner,
    });
}

std::any Registry::get_any(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = services_.find(key);
    if (it == services_.end() || it->second.empty()) {
        return {};
    }
    return it->second.back().value;
}

bool Registry::has_impl(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = services_.find(key);
    return it != services_.end() && !it->second.empty();
}

void Registry::clear_owned(OwnerToken owner) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = services_.begin(); it != services_.end();) {
        auto& entries = it->second;
        entries.erase(
            std::remove_if(
                entries.begin(),
                entries.end(),
                [owner](const Entry& entry) { return entry.owner_token == owner; }),
            entries.end());
        if (entries.empty()) {
            it = services_.erase(it);
        } else {
            ++it;
        }
    }
}

void Registry::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    services_.clear();
}

namespace detail {

std::string normalize_type_name(const std::type_info& info) {
    std::string name = info.name();
    const std::string class_kw = "class ";
    const std::string struct_kw = "struct ";

    auto erase_all = [](std::string& target, const std::string& needle) {
        std::string::size_type pos = 0;
        while ((pos = target.find(needle, pos)) != std::string::npos) {
            target.erase(pos, needle.size());
        }
    };

    erase_all(name, class_kw);
    erase_all(name, struct_kw);
    return name;
}

} // namespace detail

} // namespace server::core::util::services
