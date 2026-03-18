#pragma once

#include <any>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>

#include "server/core/util/service_registry.hpp"

namespace server::core::app {

/**
 * @brief 엔진 인스턴스 단위의 타입 기반 서비스 컨텍스트입니다.
 *
 * `util::services::Registry`가 프로세스 전역 브리지라면,
 * `EngineContext`는 한 런타임 인스턴스 안에서만 유효한 조립용 컨텍스트입니다.
 * 즉, bootstrap 단계에서 필요한 core primitive를 앱별로 모으되
 * 전역 singleton 오염 없이 같은 규약으로 구성하기 위한 용도입니다.
 */
class EngineContext {
public:
    EngineContext() = default;
    ~EngineContext() = default;

    EngineContext(const EngineContext&) = delete;
    EngineContext& operator=(const EngineContext&) = delete;

    template <typename T>
    void set(std::shared_ptr<T> service) {
        static_assert(!std::is_void_v<T>, "void 타입은 엔진 컨텍스트에 등록할 수 없습니다.");
        if (!service) {
            throw std::invalid_argument("엔진 컨텍스트에 null 서비스를 등록할 수 없습니다.");
        }
        set_any(server::core::util::services::detail::type_key<T>(), std::move(service));
    }

    template <typename T, typename... Args>
    std::shared_ptr<T> emplace(Args&&... args) {
        auto service = std::make_shared<T>(std::forward<Args>(args)...);
        set<T>(service);
        return service;
    }

    template <typename T>
    std::shared_ptr<T> get() const {
        const auto value = get_any(server::core::util::services::detail::type_key<T>());
        if (!value.has_value()) {
            return nullptr;
        }

        try {
            return std::any_cast<std::shared_ptr<T>>(value);
        } catch (const std::bad_any_cast&) {
            throw std::runtime_error(
                "엔진 컨텍스트에 등록된 서비스 타입이 요청과 일치하지 않습니다: "
                + server::core::util::services::detail::type_key<T>());
        }
    }

    template <typename T>
    T& require() const {
        auto service = get<T>();
        if (!service) {
            throw std::runtime_error(
                "엔진 컨텍스트에 요청한 서비스가 없습니다: "
                + server::core::util::services::detail::type_key<T>());
        }
        return *service;
    }

    template <typename T>
    bool has() const {
        return has_impl(server::core::util::services::detail::type_key<T>());
    }

    void clear();

private:
    void set_any(std::string key, std::any value);
    std::any get_any(const std::string& key) const;
    bool has_impl(const std::string& key) const;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::any> services_;
};

} // namespace server::core::app
