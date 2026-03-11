#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace server::core::state {

/** @brief 인스턴스 레지스트리가 보관하는 서버 한 대의 상태 스냅샷입니다. */
struct InstanceRecord {
    std::string instance_id;
    std::string host;
    std::uint16_t port{0};
    std::string role;
    std::string game_mode;
    std::string region;
    std::string shard;
    std::vector<std::string> tags;
    std::uint32_t capacity{0};
    std::uint32_t active_sessions{0};
    bool ready{true};
    std::uint64_t last_heartbeat_ms{0};
};

/** @brief 라우팅 또는 운영 정책이 인스턴스 집합을 좁힐 때 사용하는 필터입니다. */
struct InstanceSelector {
    bool all{false};
    std::vector<std::string> server_ids;
    std::vector<std::string> roles;
    std::vector<std::string> game_modes;
    std::vector<std::string> regions;
    std::vector<std::string> shards;
    std::vector<std::string> tags;
};

enum class SelectorPolicyLayer {
    kGlobal,
    kGameMode,
    kRegion,
    kShard,
    kServer,
};

/** @brief 인스턴스 선택 과정에서의 매치/미스매치 통계를 기록합니다. */
struct SelectorMatchStats {
    std::uint64_t scanned{0};
    std::uint64_t matched{0};
    std::uint64_t selector_mismatch{0};
};

bool matches_selector(const InstanceRecord& record, const InstanceSelector& selector);
SelectorPolicyLayer classify_selector_policy_layer(const InstanceSelector& selector);
std::string_view selector_policy_layer_name(SelectorPolicyLayer layer);
std::vector<InstanceRecord> select_instances(const std::vector<InstanceRecord>& instances,
                                             const InstanceSelector& selector,
                                             SelectorMatchStats* stats = nullptr);

/** @brief 인스턴스 레지스트리 저장소 구현이 따라야 할 최소 계약입니다. */
class IInstanceStateBackend {
public:
    virtual ~IInstanceStateBackend() = default;

    virtual bool upsert(const InstanceRecord& record) = 0;
    virtual bool remove(const std::string& instance_id) = 0;
    virtual bool touch(const std::string& instance_id, std::uint64_t heartbeat_ms) = 0;
    virtual std::vector<InstanceRecord> list_instances() const = 0;
};

/** @brief 테스트와 단일 프로세스 시나리오용 메모리 기반 레지스트리 구현입니다. */
class InMemoryStateBackend final : public IInstanceStateBackend {
public:
    bool upsert(const InstanceRecord& record) override;
    bool remove(const std::string& instance_id) override;
    bool touch(const std::string& instance_id, std::uint64_t heartbeat_ms) override;
    std::vector<InstanceRecord> list_instances() const override;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, InstanceRecord> records_;
};

} // namespace server::core::state
