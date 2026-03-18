#include "server/core/app/engine_context.hpp"

namespace server::core::app {

void EngineContext::set_any(std::string key, std::any value) {
    std::lock_guard<std::mutex> lock(mutex_);
    services_[std::move(key)] = std::move(value);
}

std::any EngineContext::get_any(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = services_.find(key);
    if (it == services_.end()) {
        return {};
    }
    return it->second;
}

bool EngineContext::has_impl(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return services_.find(key) != services_.end();
}

void EngineContext::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    services_.clear();
}

} // namespace server::core::app
