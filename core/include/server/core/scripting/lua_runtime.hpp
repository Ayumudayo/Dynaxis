#pragma once

#include "server/core/scripting/lua_sandbox.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace server::core::scripting {

/**
 * @brief Lua 함수와 host callback에 노출되는 hook 호출 문맥입니다.
 *
 * 스크립트는 이 구조체를 통해 session/user/room/text/command 같은 현재 호출 배경을
 * 읽습니다. 호출 문맥을 명시 구조체로 유지하는 이유는, 암묵적 전역 상태 대신
 * 어떤 정보가 스크립트 경계로 넘어가는지 계약으로 보이게 하기 위해서입니다.
 */
struct LuaHookContext {
    std::uint32_t session_id{0};
    std::string user;
    std::string room;
    std::string text;
    std::string command;
    std::string args;
    std::string issuer;
    std::string reason;
    std::string payload_json;
    std::string event;
};

enum class LuaHookDecision {
    kPass,
    kHandled,
    kBlock,
    kModify,
    kAllow,
    kDeny,
};

/**
 * @brief 서버 측 scripting hook이 사용하는 Lua runtime facade입니다.
 *
 * 이 타입은 스크립트 load/reload/call과 host API 등록을 한곳에 모으되, 실제 도메인 의미는
 * 앱 계층에 남겨 두는 메커니즘 경계입니다. 즉 reusable한 것은 runtime 자체이고,
 * chat moderation이나 room 제어 같은 의미는 이 타입 밖에서 결정합니다.
 */
class LuaRuntime {
public:
    enum class ScriptFailureKind {
        kNone,
        kInstructionLimit,
        kMemoryLimit,
        kOther,
    };

    /** @brief runtime 정책과 실행 제한값입니다. */
    struct Config {
        std::uint64_t instruction_limit{100'000};
        std::size_t memory_limit_bytes{1 * 1024 * 1024};
        std::vector<std::string> allowed_libraries{
            "base",
            "string",
            "table",
            "math",
            "utf8",
        };
    };

    /** @brief 단일 스크립트 load 결과입니다. */
    struct LoadResult {
        bool ok{false};
        std::string error;
    };

    /** @brief 단일 environment hook 호출 결과입니다. */
    struct CallResult {
        bool ok{false};
        bool executed{false};
        std::string error;
    };

    /** @brief 여러 environment를 순회하는 hook 호출 결과입니다. */
    struct CallAllResult {
        /** @brief `call_all` dispatch 안에서의 스크립트별 실행 상태입니다. */
        struct ScriptCallResult {
            std::string env_name;
            bool failed{false};
            ScriptFailureKind failure_kind{ScriptFailureKind::kNone};
        };

        std::size_t attempted{0};
        std::size_t failed{0};
        LuaHookDecision decision{LuaHookDecision::kPass};
        std::string reason;
        std::vector<std::string> notices;
        std::string error;
        std::vector<ScriptCallResult> script_results;
    };

    /** @brief reload 작업이 사용하는 스크립트 등록 엔트리입니다. */
    struct ScriptEntry {
        std::filesystem::path path;
        std::string env_name;
    };

    /** @brief Lua와 C++ host callback 사이에서 주고받는 값 표현입니다. */
    struct HostValue {
        using StringList = std::vector<std::string>;
        using Storage = std::variant<std::monostate, bool, std::int64_t, std::string, StringList>;

        Storage value{};

        HostValue() = default;
        HostValue(bool v) : value(v) {}
        HostValue(std::int64_t v) : value(v) {}
        HostValue(std::string v) : value(std::move(v)) {}
        HostValue(const char* v) : value(std::string(v ? v : "")) {}
        HostValue(StringList v) : value(std::move(v)) {}

        [[nodiscard]] bool is_nil() const {
            return std::holds_alternative<std::monostate>(value);
        }
    };

    /** @brief host callback 1회 호출에 대한 runtime/script 주석 문맥입니다. */
    struct HostCallContext {
        std::string hook_name;
        std::string script_name;
        LuaHookContext hook;
    };

    /** @brief host callback의 반환값 또는 오류입니다. */
    struct HostCallResult {
        HostValue value{};
        std::string error;
    };

    /** @brief 현재 활성 스크립트 집합을 교체한 결과입니다. */
    struct ReloadResult {
        std::size_t loaded{0};
        std::size_t failed{0};
        std::string error;
    };

    using HostArgs = std::vector<HostValue>;
    using HostCallback = std::function<HostCallResult(const HostArgs&, const HostCallContext&)>;

    /** @brief 특정 시점의 runtime counter/gauge 스냅샷입니다. */
    struct MetricsSnapshot {
        std::size_t loaded_scripts{0};
        std::size_t registered_host_api{0};
        std::size_t memory_used_bytes{0};
        std::uint64_t calls_total{0};
        std::uint64_t errors_total{0};
        std::uint64_t instruction_limit_hits{0};
        std::uint64_t memory_limit_hits{0};
        std::uint64_t reload_epoch{0};
    };

    LuaRuntime();
    explicit LuaRuntime(Config cfg);

    LoadResult load_script(const std::filesystem::path& path, const std::string& env_name);
    ReloadResult reload_scripts(const std::vector<ScriptEntry>& scripts);
    CallResult call(const std::string& env_name,
                    const std::string& func_name,
                    const LuaHookContext& ctx = {});
    CallAllResult call_all(const std::string& func_name,
                           const LuaHookContext& ctx = {});
    bool register_host_api(const std::string& table_name,
                           const std::string& func_name,
                           HostCallback callback);
    void reset();
    bool enabled() const;
    MetricsSnapshot metrics_snapshot() const;

private:
    static std::string make_api_key(std::string_view table_name, std::string_view func_name);

    mutable std::mutex mu_;
    Config cfg_;
    sandbox::Policy policy_;
    std::unordered_map<std::string, std::filesystem::path> loaded_scripts_;
    std::unordered_map<std::string, HostCallback> host_api_;
    std::size_t memory_used_bytes_{0};
    std::uint64_t calls_total_{0};
    std::uint64_t errors_total_{0};
    std::uint64_t instruction_limit_hits_{0};
    std::uint64_t memory_limit_hits_{0};
    std::uint64_t reload_epoch_{0};
};

} // namespace server::core::scripting
