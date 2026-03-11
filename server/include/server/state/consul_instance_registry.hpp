#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "server/state/instance_registry_common.hpp"

namespace server::state {

/** @brief Consul KV를 인스턴스 레지스트리 백엔드로 어댑트합니다. */
class ConsulInstanceStateBackend final : public IInstanceStateBackend {
public:
    using http_callback = std::function<bool(const std::string& path, const std::string& payload)>;

    ConsulInstanceStateBackend(std::string base_path,
                               http_callback put_callback,
                               http_callback delete_callback);

    bool upsert(const InstanceRecord& record) override;
    bool remove(const std::string& instance_id) override;
    bool touch(const std::string& instance_id, std::uint64_t heartbeat_ms) override;
    std::vector<InstanceRecord> list_instances() const override;

private:
    std::string make_path(const std::string& instance_id) const;

    std::string base_path_;
    http_callback put_;
    http_callback del_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, InstanceRecord> cache_;
};

} // namespace server::state
