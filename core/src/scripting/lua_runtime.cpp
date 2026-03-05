#include "server/core/scripting/lua_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>

#include <sol/sol.hpp>

namespace server::core::scripting {

namespace {

using HostApiMap = std::unordered_map<std::string, LuaRuntime::HostCallback>;

std::string make_disabled_error() {
    return "lua scripting is disabled at build time (BUILD_LUA_SCRIPTING=OFF)";
}

std::string trim_ascii(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size()
           && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }

    std::size_t end = value.size();
    while (end > begin
           && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return std::string(value.substr(begin, end - begin));
}

std::string to_lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::optional<std::string> extract_word_value(std::string_view text,
                                              std::string_view key) {
    const std::size_t pos = text.find(key);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }

    std::size_t begin = pos + key.size();
    while (begin < text.size()
           && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }

    std::size_t end = begin;
    while (end < text.size()
           && std::isspace(static_cast<unsigned char>(text[end])) == 0) {
        ++end;
    }

    return std::string(text.substr(begin, end - begin));
}

std::optional<std::string> extract_tail_value(std::string_view text,
                                              std::string_view key) {
    const std::size_t pos = text.find(key);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }

    std::size_t begin = pos + key.size();
    while (begin < text.size()
           && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }

    return trim_ascii(text.substr(begin));
}

bool parse_hook_decision(std::string_view token,
                         LuaHookDecision& out_decision) {
    const std::string lowered = to_lower_ascii(trim_ascii(token));
    if (lowered == "pass") {
        out_decision = LuaHookDecision::kPass;
        return true;
    }
    if (lowered == "handled") {
        out_decision = LuaHookDecision::kHandled;
        return true;
    }
    if (lowered == "block") {
        out_decision = LuaHookDecision::kBlock;
        return true;
    }
    if (lowered == "modify") {
        out_decision = LuaHookDecision::kModify;
        return true;
    }
    if (lowered == "allow") {
        out_decision = LuaHookDecision::kAllow;
        return true;
    }
    if (lowered == "deny") {
        out_decision = LuaHookDecision::kDeny;
        return true;
    }

    return false;
}

int hook_decision_rank(LuaHookDecision decision) {
    switch (decision) {
    case LuaHookDecision::kBlock:
    case LuaHookDecision::kDeny:
        return 3;
    case LuaHookDecision::kHandled:
        return 2;
    case LuaHookDecision::kModify:
        return 1;
    case LuaHookDecision::kPass:
    case LuaHookDecision::kAllow:
    default:
        return 0;
    }
}

struct ParsedDirective {
    bool present{false};
    std::string hook_name;
    std::string decision_token;
    std::string limit_token;
    std::string reason;
};

ParsedDirective parse_directive_line(std::string_view line) {
    ParsedDirective out{};

    const std::string trimmed = trim_ascii(line);
    if (trimmed.rfind("--", 0) != 0) {
        return out;
    }

    const std::string payload = trim_ascii(std::string_view(trimmed).substr(2));
    if (payload.rfind("decision=", 0) != 0
        && payload.rfind("hook=", 0) != 0
        && payload.rfind("limit=", 0) != 0) {
        return out;
    }

    if (const auto decision_token = extract_word_value(payload, "decision=");
        decision_token.has_value() && !decision_token->empty()) {
        out.decision_token = to_lower_ascii(trim_ascii(*decision_token));
    }

    if (const auto limit_token = extract_word_value(payload, "limit=");
        limit_token.has_value() && !limit_token->empty()) {
        out.limit_token = to_lower_ascii(trim_ascii(*limit_token));
    }

    if (out.decision_token.empty() && out.limit_token.empty()) {
        return out;
    }

    out.present = true;

    if (const auto hook_token = extract_word_value(payload, "hook=");
        hook_token.has_value()) {
        out.hook_name = to_lower_ascii(trim_ascii(*hook_token));
    }

    if (const auto reason_token = extract_tail_value(payload, "reason=");
        reason_token.has_value()) {
        out.reason = *reason_token;
    }

    return out;
}

struct ScriptDecisionResult {
    bool valid{true};
    LuaHookDecision decision{LuaHookDecision::kPass};
    LuaRuntime::ScriptFailureKind failure_kind{LuaRuntime::ScriptFailureKind::kNone};
    std::string reason;
    std::string notice;
    std::string error;
};

std::optional<LuaRuntime::ScriptFailureKind> parse_limit_failure_kind(std::string_view token) {
    const std::string lowered = to_lower_ascii(trim_ascii(token));
    if (lowered == "instruction"
        || lowered == "instruction_limit"
        || lowered == "instruction_limit_exceeded") {
        return LuaRuntime::ScriptFailureKind::kInstructionLimit;
    }
    if (lowered == "memory"
        || lowered == "memory_limit"
        || lowered == "memory_limit_exceeded") {
        return LuaRuntime::ScriptFailureKind::kMemoryLimit;
    }
    return std::nullopt;
}

void open_allowed_libraries(sol::state& state, const sandbox::Policy& policy) {
    if (sandbox::is_library_allowed("base", policy)) {
        state.open_libraries(sol::lib::base);
    }
    if (sandbox::is_library_allowed("string", policy)) {
        state.open_libraries(sol::lib::string);
    }
    if (sandbox::is_library_allowed("table", policy)) {
        state.open_libraries(sol::lib::table);
    }
    if (sandbox::is_library_allowed("math", policy)) {
        state.open_libraries(sol::lib::math);
    }
    if (sandbox::is_library_allowed("utf8", policy)) {
        state.open_libraries(sol::lib::utf8);
    }
    if (sandbox::is_library_allowed("coroutine", policy)) {
        state.open_libraries(sol::lib::coroutine);
    }

    if (sandbox::is_symbol_forbidden("os", policy)) {
        state["os"] = sol::nil;
    }
    if (sandbox::is_symbol_forbidden("io", policy)) {
        state["io"] = sol::nil;
    }
    if (sandbox::is_symbol_forbidden("debug", policy)) {
        state["debug"] = sol::nil;
    }
    if (sandbox::is_symbol_forbidden("package", policy)) {
        state["package"] = sol::nil;
    }
    if (sandbox::is_symbol_forbidden("ffi", policy)) {
        state["ffi"] = sol::nil;
    }
    if (sandbox::is_symbol_forbidden("dofile", policy)) {
        state["dofile"] = sol::nil;
    }
    if (sandbox::is_symbol_forbidden("loadfile", policy)) {
        state["loadfile"] = sol::nil;
    }
    if (sandbox::is_symbol_forbidden("require", policy)) {
        state["require"] = sol::nil;
    }
}

void bind_host_api_to_environment(sol::state& state,
                                  sol::environment& env,
                                  const HostApiMap& host_api) {
    std::unordered_map<std::string, sol::table> table_cache;

    for (const auto& [api_key, callback] : host_api) {
        const std::size_t dot = api_key.find('.');
        if (dot == std::string::npos || dot == 0 || (dot + 1) >= api_key.size()) {
            continue;
        }

        const std::string table_name = api_key.substr(0, dot);
        const std::string function_name = api_key.substr(dot + 1);

        auto table_it = table_cache.find(table_name);
        if (table_it == table_cache.end()) {
            sol::table table = state.create_table();
            env[table_name] = table;
            table_it = table_cache.emplace(table_name, std::move(table)).first;
        }

        const LuaRuntime::HostCallback callback_copy = callback;
        table_it->second.set_function(function_name, [callback_copy]() {
            callback_copy();
        });
    }
}

ScriptDecisionResult parse_return_table_from_lua_object(const sol::object& return_object,
                                                        std::string_view func_name) {
    ScriptDecisionResult out{};
    if (!return_object.valid() || return_object.get_type() != sol::type::table) {
        return out;
    }

    const sol::table decision_table = return_object.as<sol::table>();
    const std::string decision_token = decision_table.get_or<std::string>("decision", "");
    if (decision_token.empty()) {
        return out;
    }

    if (!parse_hook_decision(decision_token, out.decision)) {
        out.valid = false;
        out.failure_kind = LuaRuntime::ScriptFailureKind::kOther;
        out.error = "invalid decision token in return table: " + decision_token;
        return out;
    }

    const std::string hook_token = to_lower_ascii(trim_ascii(
        decision_table.get_or<std::string>("hook", "")));
    if (!hook_token.empty()) {
        const std::string target = to_lower_ascii(std::string(func_name));
        if (hook_token != target) {
            return ScriptDecisionResult{};
        }
    }

    out.reason = decision_table.get_or<std::string>("reason", "");
    out.notice = decision_table.get_or<std::string>("notice", "");
    return out;
}

struct SolHookCallResult {
    bool ok{true};
    bool executed{false};
    std::string error;
};

SolHookCallResult execute_hook_call_with_sol2(std::string_view script_text,
                                              std::string_view chunk_name,
                                              std::string_view func_name,
                                              const sandbox::Policy& policy,
                                              const HostApiMap& host_api) {
    SolHookCallResult out{};

    try {
        sol::state state;
        open_allowed_libraries(state, policy);

        sol::environment env(state, sol::create, state.globals());
        bind_host_api_to_environment(state, env, host_api);

        auto script_exec = state.safe_script(
            script_text,
            env,
            sol::script_pass_on_error,
            std::string(chunk_name),
            sol::load_mode::text);
        if (!script_exec.valid()) {
            const sol::error err = script_exec;
            out.ok = false;
            out.error = err.what();
            return out;
        }

        const sol::object hook_object = env[std::string(func_name)];
        if (!hook_object.valid() || hook_object.get_type() != sol::type::function) {
            return out;
        }

        const sol::protected_function hook = hook_object.as<sol::protected_function>();
        auto hook_exec = hook();
        out.executed = true;
        if (!hook_exec.valid()) {
            const sol::error err = hook_exec;
            out.ok = false;
            out.error = err.what();
            return out;
        }

        return out;
    } catch (const std::exception& ex) {
        out.ok = false;
        out.error = ex.what();
        return out;
    }
}

ScriptDecisionResult execute_hook_decision_with_sol2(std::string_view script_text,
                                                     std::string_view chunk_name,
                                                     std::string_view func_name,
                                                     const sandbox::Policy& policy,
                                                     const HostApiMap& host_api) {
    ScriptDecisionResult out{};

    try {
        sol::state state;
        open_allowed_libraries(state, policy);

        sol::environment env(state, sol::create, state.globals());
        bind_host_api_to_environment(state, env, host_api);

        auto script_exec = state.safe_script(
            script_text,
            env,
            sol::script_pass_on_error,
            std::string(chunk_name),
            sol::load_mode::text);
        if (!script_exec.valid()) {
            const sol::error err = script_exec;
            out.valid = false;
            out.failure_kind = LuaRuntime::ScriptFailureKind::kOther;
            out.error = err.what();
            return out;
        }

        if (script_exec.return_count() > 0) {
            out = parse_return_table_from_lua_object(
                script_exec.get<sol::object>(0),
                func_name);
        }

        const sol::object hook_object = env[std::string(func_name)];
        if (!hook_object.valid() || hook_object.get_type() != sol::type::function) {
            return out;
        }

        const sol::protected_function hook = hook_object.as<sol::protected_function>();
        auto hook_exec = hook();
        if (!hook_exec.valid()) {
            const sol::error err = hook_exec;
            ScriptDecisionResult hook_error{};
            hook_error.valid = false;
            hook_error.failure_kind = LuaRuntime::ScriptFailureKind::kOther;
            hook_error.error = err.what();
            return hook_error;
        }

        if (hook_exec.return_count() == 0) {
            return ScriptDecisionResult{};
        }

        return parse_return_table_from_lua_object(
            hook_exec.get<sol::object>(0),
            func_name);
    } catch (const std::exception& ex) {
        out.valid = false;
        out.failure_kind = LuaRuntime::ScriptFailureKind::kOther;
        out.error = ex.what();
        return out;
    }
}

std::optional<std::string> validate_script_with_sol2(const std::filesystem::path& path,
                                                     const sandbox::Policy& policy) {
    try {
        sol::state state;
        open_allowed_libraries(state, policy);
        auto load_result = state.load_file(path.string(), sol::load_mode::text);
        if (load_result.valid()) {
            return std::nullopt;
        }

        const sol::error err = load_result;
        return err.what();
    } catch (const std::exception& ex) {
        return std::string(ex.what());
    }
}

ScriptDecisionResult read_script_scaffold_decision(
    const std::filesystem::path& path,
    std::string_view func_name,
    const sandbox::Policy& policy,
    const HostApiMap& host_api) {
    std::ifstream input(path, std::ios::binary);
    if (!input.good()) {
        ScriptDecisionResult out{};
        out.valid = false;
        out.error = "failed to open script: " + path.string();
        return out;
    }

    const std::string script_text{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };

    const std::string target_hook = to_lower_ascii(std::string(func_name));
    std::istringstream lines(script_text);
    std::string line;
    while (std::getline(lines, line)) {
        const ParsedDirective directive = parse_directive_line(line);
        if (!directive.present) {
            continue;
        }

        if (!directive.hook_name.empty() && directive.hook_name != target_hook) {
            continue;
        }

        if (!directive.limit_token.empty()) {
            const auto limit_kind = parse_limit_failure_kind(directive.limit_token);
            ScriptDecisionResult out{};
            out.valid = false;
            if (!limit_kind.has_value()) {
                out.failure_kind = LuaRuntime::ScriptFailureKind::kOther;
                out.error = "invalid limit token: " + directive.limit_token + " path=" + path.string();
                return out;
            }

            out.failure_kind = *limit_kind;
            if (*limit_kind == LuaRuntime::ScriptFailureKind::kInstructionLimit) {
                out.error = "LUA_ERRRUN: instruction limit exceeded path=" + path.string();
            } else {
                out.error = "LUA_ERRMEM: memory limit exceeded path=" + path.string();
            }
            return out;
        }

        if (directive.decision_token.empty()) {
            continue;
        }

        LuaHookDecision decision = LuaHookDecision::kPass;
        if (!parse_hook_decision(directive.decision_token, decision)) {
            ScriptDecisionResult out{};
            out.valid = false;
            out.failure_kind = LuaRuntime::ScriptFailureKind::kOther;
            out.error = "invalid decision token: " + directive.decision_token + " path=" + path.string();
            return out;
        }

        ScriptDecisionResult out{};
        out.decision = decision;
        out.reason = directive.reason;
        return out;
    }

    ScriptDecisionResult table_result = execute_hook_decision_with_sol2(
        script_text,
        path.string(),
        func_name,
        policy,
        host_api);
    if (!table_result.valid) {
        table_result.error += " path=" + path.string();
    }
    return table_result;
}

} // namespace

LuaRuntime::LuaRuntime()
    : LuaRuntime(Config{}) {
}

LuaRuntime::LuaRuntime(Config cfg)
    : cfg_(std::move(cfg)),
      policy_(sandbox::default_policy()) {
    policy_.instruction_limit = cfg_.instruction_limit;
    policy_.memory_limit_bytes = cfg_.memory_limit_bytes;
    policy_.allowed_libraries = cfg_.allowed_libraries;
}

LuaRuntime::LoadResult LuaRuntime::load_script(const std::filesystem::path& path,
                                               const std::string& env_name) {
    std::lock_guard<std::mutex> lock(mu_);

    if (!enabled()) {
        ++errors_total_;
        return LoadResult{false, make_disabled_error()};
    }

    if (env_name.empty()) {
        ++errors_total_;
        return LoadResult{false, "env_name is empty"};
    }

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        ++errors_total_;
        return LoadResult{false, "script file does not exist"};
    }

    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        ++errors_total_;
        return LoadResult{false, "script path is not a regular file"};
    }

    if (const auto validation_error = validate_script_with_sol2(path, policy_);
        validation_error.has_value()) {
        ++errors_total_;
        return LoadResult{false, *validation_error};
    }

    loaded_scripts_[env_name] = path;
    return LoadResult{true, {}};
}

LuaRuntime::ReloadResult LuaRuntime::reload_scripts(const std::vector<ScriptEntry>& scripts) {
    std::lock_guard<std::mutex> lock(mu_);

    if (!enabled()) {
        ++errors_total_;
        return ReloadResult{0, scripts.size(), make_disabled_error()};
    }

    std::unordered_map<std::string, std::filesystem::path> reloaded;
    reloaded.reserve(scripts.size());

    std::size_t failed = 0;
    for (const auto& script : scripts) {
        if (script.env_name.empty()) {
            ++errors_total_;
            ++failed;
            continue;
        }

        std::error_code ec;
        if (!std::filesystem::exists(script.path, ec) || ec) {
            ++errors_total_;
            ++failed;
            continue;
        }
        if (!std::filesystem::is_regular_file(script.path, ec) || ec) {
            ++errors_total_;
            ++failed;
            continue;
        }

        if (const auto validation_error = validate_script_with_sol2(script.path, policy_);
            validation_error.has_value()) {
            ++errors_total_;
            ++failed;
            continue;
        }

        auto [it, inserted] = reloaded.emplace(script.env_name, script.path);
        if (!inserted) {
            ++errors_total_;
            ++failed;
            it->second = script.path;
        }
    }

    loaded_scripts_ = std::move(reloaded);
    ++reload_epoch_;
    return ReloadResult{loaded_scripts_.size(), failed, {}};
}

LuaRuntime::CallResult LuaRuntime::call(const std::string& env_name,
                                        const std::string& func_name) {
    std::lock_guard<std::mutex> lock(mu_);

    if (!enabled()) {
        ++errors_total_;
        return CallResult{false, false, make_disabled_error()};
    }

    if (func_name.empty()) {
        ++errors_total_;
        return CallResult{false, false, "func_name is empty"};
    }

    const auto it = loaded_scripts_.find(env_name);
    if (it == loaded_scripts_.end()) {
        return CallResult{true, false, {}};
    }

    std::ifstream input(it->second, std::ios::binary);
    if (!input.good()) {
        ++errors_total_;
        return CallResult{false, false, "failed to open script: " + it->second.string()};
    }

    const std::string script_text{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };

    const auto hook_call = execute_hook_call_with_sol2(
        script_text,
        it->second.string(),
        func_name,
        policy_,
        host_api_);
    if (!hook_call.ok) {
        ++errors_total_;
        return CallResult{false, hook_call.executed, hook_call.error};
    }

    ++calls_total_;
    return CallResult{true, hook_call.executed, {}};
}

LuaRuntime::CallAllResult LuaRuntime::call_all(const std::string& func_name) {
    std::lock_guard<std::mutex> lock(mu_);

    if (!enabled()) {
        ++errors_total_;
        return CallAllResult{
            0,
            loaded_scripts_.size(),
            LuaHookDecision::kPass,
            {},
            {},
            make_disabled_error(),
            {},
        };
    }

    if (func_name.empty()) {
        ++errors_total_;
        return CallAllResult{0, 0, LuaHookDecision::kPass, {}, {}, "func_name is empty", {}};
    }

    const std::size_t attempted = loaded_scripts_.size();
    std::size_t failed = 0;
    LuaHookDecision aggregated_decision = LuaHookDecision::kPass;
    std::string aggregated_reason;
    std::vector<std::string> aggregated_notices;
    std::string first_error;
    std::vector<CallAllResult::ScriptCallResult> script_results;
    script_results.reserve(loaded_scripts_.size());

    std::vector<std::pair<std::string, std::filesystem::path>> ordered_scripts;
    ordered_scripts.reserve(loaded_scripts_.size());
    for (const auto& entry : loaded_scripts_) {
        ordered_scripts.emplace_back(entry.first, entry.second);
    }
    std::sort(ordered_scripts.begin(), ordered_scripts.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.first < rhs.first;
    });

    for (const auto& [env_name, path] : ordered_scripts) {
        CallAllResult::ScriptCallResult script_call{};
        script_call.env_name = env_name;

        const auto script_result = read_script_scaffold_decision(
            path,
            func_name,
            policy_,
            host_api_);
        if (!script_result.valid) {
            script_call.failed = true;
            switch (script_result.failure_kind) {
            case LuaRuntime::ScriptFailureKind::kInstructionLimit:
                script_call.failure_kind = LuaRuntime::ScriptFailureKind::kInstructionLimit;
                ++instruction_limit_hits_;
                break;
            case LuaRuntime::ScriptFailureKind::kMemoryLimit:
                script_call.failure_kind = LuaRuntime::ScriptFailureKind::kMemoryLimit;
                ++memory_limit_hits_;
                break;
            case LuaRuntime::ScriptFailureKind::kNone:
            case LuaRuntime::ScriptFailureKind::kOther:
            default:
                script_call.failure_kind = LuaRuntime::ScriptFailureKind::kOther;
                break;
            }
            script_results.push_back(std::move(script_call));

            ++failed;
            ++errors_total_;
            if (first_error.empty()) {
                first_error = script_result.error;
            }
            continue;
        }

        script_results.push_back(std::move(script_call));

        if (!script_result.notice.empty()) {
            aggregated_notices.push_back(script_result.notice);
        }

        const int current_rank = hook_decision_rank(aggregated_decision);
        const int candidate_rank = hook_decision_rank(script_result.decision);
        if (candidate_rank > current_rank) {
            aggregated_decision = script_result.decision;
            aggregated_reason = script_result.reason;
        }
    }

    calls_total_ += attempted;
    return CallAllResult{
        attempted,
        failed,
        aggregated_decision,
        aggregated_reason,
        aggregated_notices,
        first_error,
        std::move(script_results),
    };
}

bool LuaRuntime::register_host_api(const std::string& table_name,
                                   const std::string& func_name,
                                   HostCallback callback) {
    std::lock_guard<std::mutex> lock(mu_);

    if (!enabled()) {
        ++errors_total_;
        return false;
    }

    if (table_name.empty() || func_name.empty() || !callback) {
        ++errors_total_;
        return false;
    }

    host_api_[make_api_key(table_name, func_name)] = std::move(callback);
    return true;
}

void LuaRuntime::reset() {
    std::lock_guard<std::mutex> lock(mu_);

    loaded_scripts_.clear();
    host_api_.clear();
    calls_total_ = 0;
    errors_total_ = 0;
    instruction_limit_hits_ = 0;
    memory_limit_hits_ = 0;
    reload_epoch_ = 0;
}

bool LuaRuntime::enabled() const {
#if KNIGHTS_BUILD_LUA_SCRIPTING
    return true;
#else
    return false;
#endif
}

LuaRuntime::MetricsSnapshot LuaRuntime::metrics_snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);

    MetricsSnapshot snapshot{};
    snapshot.loaded_scripts = loaded_scripts_.size();
    snapshot.registered_host_api = host_api_.size();
    snapshot.memory_used_bytes = 0;
    snapshot.calls_total = calls_total_;
    snapshot.errors_total = errors_total_;
    snapshot.instruction_limit_hits = instruction_limit_hits_;
    snapshot.memory_limit_hits = memory_limit_hits_;
    snapshot.reload_epoch = reload_epoch_;
    return snapshot;
}

std::string LuaRuntime::make_api_key(std::string_view table_name,
                                     std::string_view func_name) {
    std::string key;
    key.reserve(table_name.size() + func_name.size() + 1);
    key.append(table_name);
    key.push_back('.');
    key.append(func_name);
    return key;
}

} // namespace server::core::scripting
