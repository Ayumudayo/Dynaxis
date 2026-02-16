#include "server/state/instance_registry.hpp"

#include <string_view>
#include <cctype>
#include <sstream>
#include <utility>

#include "server/storage/redis/client.hpp"

namespace server::state {

namespace {
constexpr const char* kContentTypeHeader = "application/json";
} // namespace

// -----------------------------------------------------------------------------
// InMemoryStateBackend 구현
// -----------------------------------------------------------------------------
// 테스트나 단일 프로세스 환경에서 사용되는 인메모리 레지스트리입니다.
// 별도의 외부 저장소 없이 메모리 상의 맵(Map)으로 인스턴스 정보를 관리합니다.

bool InMemoryStateBackend::upsert(const InstanceRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    records_[record.instance_id] = record;
    return true;
}

bool InMemoryStateBackend::remove(const std::string& instance_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_.erase(instance_id) > 0;
}

bool InMemoryStateBackend::touch(const std::string& instance_id, std::uint64_t heartbeat_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = records_.find(instance_id);
    if (it == records_.end()) {
        return false;
    }
    it->second.last_heartbeat_ms = heartbeat_ms;
    return true;
}

std::vector<InstanceRecord> InMemoryStateBackend::list_instances() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<InstanceRecord> result;
    result.reserve(records_.size());
    for (const auto& [_, record] : records_) {
        result.push_back(record);
    }
    return result;
}

namespace detail {

// Redis registry와 통신하기 위해 간단한 JSON 직렬화를 구현한다.
// 외부 라이브러리 의존성을 줄이기 위해 직접 문자열 파싱/생성을 수행합니다.
// 성능보다는 이식성과 의존성 최소화에 중점을 둔 구현입니다.
std::string serialize_json(const InstanceRecord& record) {
    std::ostringstream oss;
    oss << '{';
    oss << "\"instance_id\":\"" << record.instance_id << "\",";
    oss << "\"host\":\"" << record.host << "\",";
    oss << "\"port\":" << record.port << ',';
    oss << "\"role\":\"" << record.role << "\",";
    oss << "\"capacity\":" << record.capacity << ',';
    oss << "\"active_sessions\":" << record.active_sessions << ',';
    oss << "\"ready\":" << (record.ready ? "true" : "false") << ',';
    oss << "\"last_heartbeat_ms\":" << record.last_heartbeat_ms;
    oss << '}';
    return oss.str();
}

// Redis에서 가져온 문자열을 InstanceRecord로 역직렬화한다.
// 단순한 JSON 파서로, 중첩된 객체나 배열은 처리하지 않습니다.
std::optional<InstanceRecord> deserialize_json(std::string_view payload) {
    InstanceRecord record{};
    std::string json(payload);
    // 괄호 제거
    for (char& ch : json) {
        if (ch == '{' || ch == '}') {
            ch = ' ';
        }
    }
    // 공백 제거 람다
    auto trim_copy = [](std::string_view view) -> std::string {
        std::size_t start = 0;
        std::size_t end = view.size();
        while (start < end && std::isspace(static_cast<unsigned char>(view[start]))) {
            ++start;
        }
        while (end > start && std::isspace(static_cast<unsigned char>(view[end - 1]))) {
            --end;
        }
        return std::string(view.substr(start, end - start));
    };
    std::stringstream ss(json);
    std::string item;
    // 쉼표로 구분된 키-값 쌍을 파싱
    while (std::getline(ss, item, ',')) {
        auto colon = item.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        std::string key = trim_copy(std::string_view(item).substr(0, colon));
        std::string value = trim_copy(std::string_view(item).substr(colon + 1));
        // 따옴표 제거
        if (!key.empty() && key.front() == '"') { key.erase(0, 1); }
        if (!key.empty() && key.back() == '"') { key.pop_back(); }
        if (!value.empty() && value.front() == '"') { value.erase(0, 1); }
        if (!value.empty() && value.back() == '"') { value.pop_back(); }
        
        try {
            if (key == "instance_id") {
                record.instance_id = value;
            } else if (key == "host") {
                record.host = value;
            } else if (key == "role") {
                record.role = value;
            } else if (key == "port") {
                record.port = static_cast<std::uint16_t>(std::stoul(value));
            } else if (key == "capacity") {
                record.capacity = static_cast<std::uint32_t>(std::stoul(value));
            } else if (key == "active_sessions") {
                record.active_sessions = static_cast<std::uint32_t>(std::stoul(value));
            } else if (key == "ready") {
                if (value == "true" || value == "1") {
                    record.ready = true;
                } else if (value == "false" || value == "0") {
                    record.ready = false;
                }
            } else if (key == "last_heartbeat_ms") {
                record.last_heartbeat_ms = static_cast<std::uint64_t>(std::stoull(value));
            }
        } catch (...) {
            // 파싱 에러 무시 (유연한 처리)
        }
    }
    return record;
}

} // namespace detail

namespace {

// 기존 redis::IRedisClient를 instance registry 인터페이스로 감싸는 어댑터.
// 의존성 주입을 용이하게 하기 위해 사용됩니다.
class RedisClientAdapter final : public RedisInstanceStateBackend::IRedisClient {
public:
    explicit RedisClientAdapter(std::shared_ptr<server::storage::redis::IRedisClient> client)
        : client_(std::move(client)) {}

    bool scan_keys(const std::string& pattern, std::vector<std::string>& keys) override {
        if (!client_) {
            return false;
        }
        return client_->scan_keys(pattern, keys);
    }

    std::optional<std::string> get(const std::string& key) override {
        if (!client_) {
            return std::nullopt;
        }
        return client_->get(key);
    }

    bool setex(const std::string& key, const std::string& value, unsigned int ttl_sec) override {
        if (!client_) {
            return false;
        }
        return client_->setex(key, value, ttl_sec);
    }

    bool del(const std::string& key) override {
        if (!client_) {
            return false;
        }
        return client_->del(key);
    }

private:
    std::shared_ptr<server::storage::redis::IRedisClient> client_;
};

} // namespace

// -----------------------------------------------------------------------------
// RedisInstanceStateBackend 구현
// -----------------------------------------------------------------------------
// Redis를 백엔드로 사용하는 인스턴스 레지스트리입니다.
// 각 인스턴스는 고유한 키에 JSON 형태로 저장되며, TTL(Time-To-Live)을 통해
// 비정상 종료 시 자동으로 목록에서 제거됩니다. (Heartbeat 패턴)
// 이를 통해 별도의 헬스 체크 로직 없이도 죽은 서버를 감지할 수 있습니다.

RedisInstanceStateBackend::RedisInstanceStateBackend(std::shared_ptr<IRedisClient> client,
                                                     std::string key_prefix,
                                                     std::chrono::seconds ttl)
    : client_(std::move(client))
    , key_prefix_(std::move(key_prefix))
    , ttl_(ttl) {
    if (key_prefix_.empty()) {
        key_prefix_ = "gateway/instances/";
    }
    if (key_prefix_.back() != '/') {
        key_prefix_.push_back('/');
    }
    if (ttl_ <= std::chrono::seconds::zero()) {
        ttl_ = std::chrono::seconds{10};
    }
}

// 인스턴스 정보를 갱신하거나 새로 등록합니다.
// 로컬 캐시를 먼저 갱신하고, Redis에 쓰기 작업을 수행합니다.
bool RedisInstanceStateBackend::upsert(const InstanceRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_[record.instance_id] = record;
    return write_record(record);
}

// 인스턴스를 목록에서 제거합니다.
bool RedisInstanceStateBackend::remove(const std::string& instance_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.erase(instance_id);
    if (!client_) {
        return true;
    }
    return client_->del(key_prefix_ + instance_id);
}

// 하트비트 시각만 갱신하여 Redis 키의 TTL을 연장합니다.
// 전체 레코드를 다시 쓰지만, 이는 TTL 갱신을 위한 가장 확실한 방법입니다.
bool RedisInstanceStateBackend::touch(const std::string& instance_id, std::uint64_t heartbeat_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(instance_id);
    if (it == cache_.end()) {
        return false;
    }
    it->second.last_heartbeat_ms = heartbeat_ms;
    return write_record(it->second);
}

// Redis의 모든 인스턴스 키를 스캔하여 로컬 캐시를 재구성합니다.
// 게이트웨이 재시작 시 또는 주기적인 동기화에 사용됩니다.
// SCAN 명령어를 사용하여 대량의 키가 있어도 Redis를 블로킹하지 않고 안전하게 조회합니다.
bool RedisInstanceStateBackend::reload_cache_from_backend() const {
    if (!client_) {
        return false;
    }
    std::vector<std::string> keys;
    if (!client_->scan_keys(key_prefix_ + "*", keys)) {
        return false;
    }
    std::unordered_map<std::string, InstanceRecord> next;
    next.reserve(keys.size());
    for (const auto& key : keys) {
        auto payload = client_->get(key);
        if (!payload || payload->empty()) {
            continue;
        }
        auto record = detail::deserialize_json(*payload);
        if (!record) {
            continue;
        }
        // ID가 누락된 경우 키에서 추출
        if (record->instance_id.empty() && key.size() > key_prefix_.size()) {
            record->instance_id = key.substr(key_prefix_.size());
        }
        next[record->instance_id] = *record;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_ = std::move(next);
    }
    return true;
}

std::vector<InstanceRecord> RedisInstanceStateBackend::list_instances() const {
    if (client_) {
        const auto now = std::chrono::steady_clock::now();
        bool should_reload = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (now - last_reload_attempt_ >= reload_min_interval_) {
                last_reload_attempt_ = now;
                should_reload = true;
            }
        }

        if (should_reload) {
            bool expected = false;
            if (reload_in_progress_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                const bool ok = reload_cache_from_backend();
                reload_in_progress_.store(false, std::memory_order_release);
                if (ok) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    last_reload_ok_ = now;
                }
            }
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<InstanceRecord> result;
    result.reserve(cache_.size());
    for (const auto& [_, record] : cache_) {
        result.push_back(record);
    }
    return result;
}

// Redis에 JSON을 setex로 저장해 TTL 기반 heartbeat를 유지한다.
bool RedisInstanceStateBackend::write_record(const InstanceRecord& record) {
    if (!client_) {
        return true;
    }
    const auto key = key_prefix_ + record.instance_id;
    const auto payload = detail::serialize_json(record);
    const auto ttl = static_cast<unsigned int>(ttl_.count());
    return client_->setex(key, payload, ttl);
}

// -----------------------------------------------------------------------------
// ConsulInstanceStateBackend 구현
// -----------------------------------------------------------------------------
// Consul KV 저장소를 이용한 백엔드입니다. (현재는 HTTP 콜백으로 추상화됨)

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

std::shared_ptr<RedisInstanceStateBackend::IRedisClient>
make_redis_state_client(std::shared_ptr<server::storage::redis::IRedisClient> client) {
    return std::make_shared<RedisClientAdapter>(std::move(client));
}

} // namespace server::state
