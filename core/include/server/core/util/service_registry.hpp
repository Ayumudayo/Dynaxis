#pragma once

#include <any>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <vector>

namespace server::core::util::services {

namespace detail {

std::string normalize_type_name(const std::type_info& info);

template <typename T>
std::string type_key() {
    static_assert(!std::is_void_v<T>, "void 타입은 서비스 레지스트리에 등록할 수 없습니다.");
    // 컴파일러/플랫폼별 type name 표현 차이를 normalize 하여
    // 모듈 경계를 넘어도 동일한 키 문자열을 얻도록 한다.
    return normalize_type_name(typeid(T));
}

} // namespace detail

/**
 * @brief 전역 서비스 레지스트리 (Service Locator 패턴)
 * 
 * 애플리케이션 전역에서 공유해야 하는 서비스 객체(싱글톤 등)를 타입 기반으로 관리합니다.
 * 의존성 주입(DI) 컨테이너의 단순화된 형태입니다.
 *
 * 왜 필요한가?
 * - server/gateway/tools처럼 여러 바이너리에서 공통 런타임 객체(로거, 설정, 스토리지 핸들 등)를
 *   일관된 방식으로 주입/조회할 수 있습니다.
 * - 복잡한 외부 DI 프레임워크 없이도 타입 안정성과 중앙 관리점을 확보할 수 있습니다.
 *
 * 설계 포인트:
 * - 내부 저장소는 std::any + std::shared_ptr<T> 조합을 사용합니다.
 *   즉, "수명은 shared_ptr로 안전하게", "조회 타입은 any_cast로 검증"하는 구조입니다.
 * 
 * 예:
 * services::set(std::make_shared<MyService>());
 * auto service = services::get<MyService>();
 */
class Registry {
public:
    using OwnerToken = std::uintptr_t;
    static constexpr OwnerToken kUnowned = 0;

    static Registry& instance();

    template <typename T>
    void set(std::shared_ptr<T> service) {
        static_assert(!std::is_void_v<T>, "void 타입은 서비스 레지스트리에 등록할 수 없습니다.");
        if (!service) {
            throw std::invalid_argument("서비스 포인터가 null 입니다.");
        }
        set_any(detail::type_key<T>(), std::move(service), kUnowned);
    }

    template <typename T>
    void set_owned(OwnerToken owner, std::shared_ptr<T> service) {
        static_assert(!std::is_void_v<T>, "void 타입은 서비스 레지스트리에 등록할 수 없습니다.");
        if (!service) {
            throw std::invalid_argument("서비스 포인터가 null 입니다.");
        }
        set_any(detail::type_key<T>(), std::move(service), owner);
    }

    template <typename T, typename... Args>
    std::shared_ptr<T> emplace(Args&&... args) {
        auto service = std::make_shared<T>(std::forward<Args>(args)...);
        set<T>(service);
        return service;
    }

    template <typename T>
    std::shared_ptr<T> get() const {
        auto value = get_any(detail::type_key<T>());
        if (!value.has_value()) {
            return nullptr;
        }
        try {
            return std::any_cast<std::shared_ptr<T>>(value);
        } catch (const std::bad_any_cast&) {
            // 같은 키에 잘못된 타입이 들어간 경우는 프로그래밍 오류이므로 즉시 예외로 노출한다.
            throw std::runtime_error("요청한 타입과 등록된 서비스 타입이 일치하지 않습니다: " + detail::type_key<T>());
        }
    }

    template <typename T>
    T& require() const {
        auto ptr = get<T>();
        if (!ptr) {
            throw std::runtime_error("요청한 서비스가 등록되지 않았습니다: " + detail::type_key<T>());
        }
        return *ptr;
    }

    template <typename T>
    bool has() const {
        return has_impl(detail::type_key<T>());
    }

    void clear_owned(OwnerToken owner);
    void clear();

private:
    struct Entry {
        std::any value;
        OwnerToken owner_token{kUnowned};
    };

    Registry() = default;

    void set_any(std::string key, std::any value, OwnerToken owner);
    std::any get_any(const std::string& key) const;
    bool has_impl(const std::string& key) const;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::vector<Entry>> services_;
};

template <typename T>
void set(std::shared_ptr<T> service) {
    Registry::instance().set<T>(std::move(service));
}

template <typename T, typename... Args>
std::shared_ptr<T> emplace(Args&&... args) {
    return Registry::instance().emplace<T>(std::forward<Args>(args)...);
}

template <typename T>
std::shared_ptr<T> get() {
    return Registry::instance().get<T>();
}

template <typename T>
T& require() {
    return Registry::instance().require<T>();
}

template <typename T>
bool has() {
    return Registry::instance().has<T>();
}

inline void clear() {
    Registry::instance().clear();
}

} // namespace server::core::util::services
