#include "server/state/consul_instance_registry.hpp"

#include <utility>

namespace server::state {

ConsulInstanceStateBackend::ConsulInstanceStateBackend(std::string base_path,
                                                       http_callback put_callback,
                                                       http_callback delete_callback)
    : base_path_(std::move(base_path))
    , put_(std::move(put_callback))
    , del_(std::move(delete_callback)) {
    if (base_path_.empty()) {
        base_path_ = "kv/gateway/instances/";
    }
    if (base_path_.back() != '/') {
        base_path_.push_back('/');
    }
}

bool ConsulInstanceStateBackend::upsert(const InstanceRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_[record.instance_id] = record;
    if (!put_) {
        return true;
    }
    const auto payload = detail::serialize_json(record);
    return put_(make_path(record.instance_id), payload);
}

bool ConsulInstanceStateBackend::remove(const std::string& instance_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.erase(instance_id);
    if (!del_) {
        return true;
    }
    return del_(make_path(instance_id), "");
}

bool ConsulInstanceStateBackend::touch(const std::string& instance_id, std::uint64_t heartbeat_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(instance_id);
    if (it == cache_.end()) {
        return false;
    }
    it->second.last_heartbeat_ms = heartbeat_ms;
    if (!put_) {
        return true;
    }
    const auto payload = detail::serialize_json(it->second);
    return put_(make_path(instance_id), payload);
}

std::vector<InstanceRecord> ConsulInstanceStateBackend::list_instances() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<InstanceRecord> result;
    result.reserve(cache_.size());
    for (const auto& [_, record] : cache_) {
        result.push_back(record);
    }
    return result;
}

std::string ConsulInstanceStateBackend::make_path(const std::string& instance_id) const {
    return base_path_ + instance_id;
}

} // namespace server::state
