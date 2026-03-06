#include "server/core/scripting/lua_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

#include <lua.hpp>
#include <sol/sol.hpp>

#include "luajit_rolling.h"

namespace server::core::scripting {

namespace {

using HostApiMap = std::unordered_map<std::string, LuaRuntime::HostCallback>;

constexpr int kInstructionHookGranularity = 100;

struct VmExecutionBudget {
    std::size_t memory_limit_bytes{0};
    std::size_t used_bytes{0};
    std::size_t peak_bytes{0};
    std::uint64_t instruction_limit{0};
    std::uint64_t executed_instructions{0};
    bool memory_limit_hit{false};
    bool instruction_limit_hit{false};
};

struct ScriptDecisionResult {
    bool valid{true};
    bool executed{false};
    LuaHookDecision decision{LuaHookDecision::kPass};
    LuaRuntime::ScriptFailureKind failure_kind{LuaRuntime::ScriptFailureKind::kNone};
    std::string reason;
    std::string notice;
    std::string error;
    std::size_t peak_memory_bytes{0};
};

struct ParsedDirective {
    bool present{false};
    std::string hook_name;
    std::string decision_token;
    std::string limit_token;
    std::string reason;
};

struct LuaStateCloser {
    void operator()(lua_State* state) const {
        if (state != nullptr) {
            lua_close(state);
        }
    }
};

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

std::optional<std::string> read_script_text(const std::filesystem::path& path, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input.good()) {
        error = "failed to open script: " + path.string();
        return std::nullopt;
    }

    return std::string{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };
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

std::optional<ParsedDirective> find_matching_directive(std::string_view script_text,
                                                       std::string_view func_name) {
    const std::string target_hook = to_lower_ascii(std::string(func_name));

    std::istringstream lines{std::string(script_text)};
    std::string line;
    while (std::getline(lines, line)) {
        const ParsedDirective directive = parse_directive_line(line);
        if (!directive.present) {
            continue;
        }
        if (!directive.hook_name.empty() && directive.hook_name != target_hook) {
            continue;
        }
        return directive;
    }

    return std::nullopt;
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

void* budgeted_lua_alloc(void* ud, void* ptr, std::size_t, std::size_t nsize) {
    auto& budget = *static_cast<VmExecutionBudget*>(ud);

    if (nsize == 0) {
        if (ptr != nullptr) {
            auto* raw = static_cast<std::size_t*>(ptr) - 1;
            budget.used_bytes -= *raw;
            std::free(raw);
        }
        return nullptr;
    }

    const std::size_t old_size = ptr == nullptr ? 0 : *(static_cast<std::size_t*>(ptr) - 1);
    if (budget.memory_limit_bytes > 0) {
        const std::size_t used_without_old = budget.used_bytes - old_size;
        if (nsize > budget.memory_limit_bytes
            || used_without_old > (budget.memory_limit_bytes - nsize)) {
            budget.memory_limit_hit = true;
            return nullptr;
        }
    }

    void* raw = nullptr;
    if (ptr == nullptr) {
        raw = std::malloc(sizeof(std::size_t) + nsize);
    } else {
        raw = std::realloc(static_cast<std::size_t*>(ptr) - 1, sizeof(std::size_t) + nsize);
    }

    if (raw == nullptr) {
        return nullptr;
    }

    auto* sized = static_cast<std::size_t*>(raw);
    sized[0] = nsize;
    budget.used_bytes = budget.used_bytes - old_size + nsize;
    budget.peak_bytes = std::max(budget.peak_bytes, budget.used_bytes);
    return sized + 1;
}

void instruction_limit_hook(lua_State* state, lua_Debug*) {
    void* ud = nullptr;
    (void)lua_getallocf(state, &ud);
    auto& budget = *static_cast<VmExecutionBudget*>(ud);

    budget.executed_instructions += kInstructionHookGranularity;
    if (budget.instruction_limit > 0
        && budget.executed_instructions > budget.instruction_limit) {
        budget.instruction_limit_hit = true;
        luaL_error(state, "instruction limit exceeded");
    }
}

std::unique_ptr<lua_State, LuaStateCloser> create_state(VmExecutionBudget& budget,
                                                        const sandbox::Policy& policy) {
    lua_State* raw = lua_newstate(&budgeted_lua_alloc, &budget);
    if (raw == nullptr) {
        return {};
    }

    if (policy.instruction_limit > 0) {
        lua_sethook(raw,
                    &instruction_limit_hook,
                    LUA_MASKCOUNT,
                    kInstructionHookGranularity);
    }

    (void)luaJIT_setmode(raw, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF);
    return std::unique_ptr<lua_State, LuaStateCloser>(raw);
}

void open_allowed_libraries(sol::state_view& state, const sandbox::Policy& policy) {
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

LuaRuntime::HostValue host_value_from_lua_object(const sol::object& object, std::string& error) {
    switch (object.get_type()) {
    case sol::type::none:
    case sol::type::lua_nil:
        return {};
    case sol::type::boolean:
        return LuaRuntime::HostValue{object.as<bool>()};
    case sol::type::number:
        return LuaRuntime::HostValue{static_cast<std::int64_t>(object.as<lua_Integer>())};
    case sol::type::string:
        return LuaRuntime::HostValue{object.as<std::string>()};
    case sol::type::table: {
        LuaRuntime::HostValue::StringList values;
        const sol::table table = object.as<sol::table>();
        const std::size_t size = table.size();
        values.reserve(size);
        for (std::size_t index = 1; index <= size; ++index) {
            const sol::object item = table.get<sol::object>(index);
            if (!item.valid() || item.get_type() != sol::type::string) {
                error = "only string arrays are supported for table arguments";
                return {};
            }
            values.push_back(item.as<std::string>());
        }
        return LuaRuntime::HostValue{std::move(values)};
    }
    default:
        error = "unsupported Lua argument type";
        return {};
    }
}

sol::object host_value_to_lua_object(sol::state_view& state,
                                     const LuaRuntime::HostValue& value) {
    if (std::holds_alternative<std::monostate>(value.value)) {
        return sol::make_object(state, sol::nil);
    }
    if (const auto* boolean = std::get_if<bool>(&value.value)) {
        return sol::make_object(state, *boolean);
    }
    if (const auto* integer = std::get_if<std::int64_t>(&value.value)) {
        return sol::make_object(state, *integer);
    }
    if (const auto* string = std::get_if<std::string>(&value.value)) {
        return sol::make_object(state, *string);
    }
    if (const auto* list = std::get_if<LuaRuntime::HostValue::StringList>(&value.value)) {
        sol::table table = state.create_table();
        std::size_t index = 1;
        for (const auto& item : *list) {
            table[index++] = item;
        }
        return sol::make_object(state, table);
    }
    return sol::make_object(state, sol::nil);
}

sol::table build_context_table(sol::state_view& state,
                               const LuaHookContext& ctx) {
    sol::table table = state.create_table();
    if (ctx.session_id != 0) {
        table["session_id"] = ctx.session_id;
    }
    if (!ctx.user.empty()) {
        table["user"] = ctx.user;
    }
    if (!ctx.room.empty()) {
        table["room"] = ctx.room;
    }
    if (!ctx.text.empty()) {
        table["text"] = ctx.text;
    }
    if (!ctx.command.empty()) {
        table["command"] = ctx.command;
    }
    if (!ctx.args.empty()) {
        table["args"] = ctx.args;
    }
    if (!ctx.issuer.empty()) {
        table["issuer"] = ctx.issuer;
    }
    if (!ctx.reason.empty()) {
        table["reason"] = ctx.reason;
    }
    if (!ctx.payload_json.empty()) {
        table["payload_json"] = ctx.payload_json;
    }
    if (!ctx.event.empty()) {
        table["event"] = ctx.event;
    }
    return table;
}

void bind_host_api_to_environment(sol::state_view& state,
                                  sol::environment& env,
                                  const HostApiMap& host_api,
                                  const LuaRuntime::HostCallContext& context) {
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
        const LuaRuntime::HostCallContext context_copy = context;
        table_it->second.set_function(
            function_name,
            [callback_copy, context_copy](sol::this_state this_state, sol::variadic_args values) -> sol::object {
                sol::state_view state(this_state);
                LuaRuntime::HostArgs args;
                for (auto value : values) {
                    std::string error;
                    const LuaRuntime::HostValue converted = host_value_from_lua_object(value, error);
                    if (!error.empty()) {
                        luaL_error(state.lua_state(), "%s", error.c_str());
                        return sol::make_object(state, sol::nil);
                    }
                    args.push_back(converted);
                }

                const LuaRuntime::HostCallResult result = callback_copy(args, context_copy);
                if (!result.error.empty()) {
                    luaL_error(state.lua_state(), "%s", result.error.c_str());
                    return sol::make_object(state, sol::nil);
                }
                return host_value_to_lua_object(state, result.value);
            });
    }
}

std::optional<LuaRuntime::ScriptFailureKind> classify_failure(const VmExecutionBudget& budget) {
    if (budget.instruction_limit_hit) {
        return LuaRuntime::ScriptFailureKind::kInstructionLimit;
    }
    if (budget.memory_limit_hit) {
        return LuaRuntime::ScriptFailureKind::kMemoryLimit;
    }
    return std::nullopt;
}

std::optional<std::string> validate_script_with_lua(const std::filesystem::path& path,
                                                    const sandbox::Policy& policy,
                                                    std::size_t& peak_memory_bytes) {
    std::string error;
    const auto script_text = read_script_text(path, error);
    if (!script_text.has_value()) {
        return error;
    }

    VmExecutionBudget budget{};
    budget.memory_limit_bytes = policy.memory_limit_bytes;
    budget.instruction_limit = policy.instruction_limit;

    auto state_owner = create_state(budget, policy);
    if (!state_owner) {
        peak_memory_bytes = budget.peak_bytes;
        return std::string("failed to create lua state");
    }

    const int status = luaL_loadbufferx(
        state_owner.get(),
        script_text->data(),
        script_text->size(),
        path.string().c_str(),
        "t");
    peak_memory_bytes = budget.peak_bytes;
    if (status == LUA_OK) {
        lua_pop(state_owner.get(), 1);
        return std::nullopt;
    }

    std::string message = lua_tostring(state_owner.get(), -1);
    lua_pop(state_owner.get(), 1);

    if (const auto failure_kind = classify_failure(budget); failure_kind.has_value()) {
        if (*failure_kind == LuaRuntime::ScriptFailureKind::kInstructionLimit) {
            return std::string("instruction limit exceeded while validating: ") + path.string();
        }
        return std::string("memory limit exceeded while validating: ") + path.string();
    }
    return message;
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

ScriptDecisionResult execute_script(std::string_view script_text,
                                    std::string_view chunk_name,
                                    std::string_view env_name,
                                    std::string_view func_name,
                                    const LuaHookContext& hook_context,
                                    const sandbox::Policy& policy,
                                    const HostApiMap& host_api) {
    ScriptDecisionResult out{};

    if (const auto directive = find_matching_directive(script_text, func_name);
        directive.has_value() && !directive->limit_token.empty()) {
        const auto limit_kind = parse_limit_failure_kind(directive->limit_token);
        if (!limit_kind.has_value()) {
            out.valid = false;
            out.failure_kind = LuaRuntime::ScriptFailureKind::kOther;
            out.error = "invalid limit token: " + directive->limit_token;
            return out;
        }

        out.valid = false;
        out.failure_kind = *limit_kind;
        out.error = (*limit_kind == LuaRuntime::ScriptFailureKind::kInstructionLimit)
            ? "LUA_ERRRUN: instruction limit exceeded"
            : "LUA_ERRMEM: memory limit exceeded";
        return out;
    }

    VmExecutionBudget budget{};
    budget.memory_limit_bytes = policy.memory_limit_bytes;
    budget.instruction_limit = policy.instruction_limit;

    auto state_owner = create_state(budget, policy);
    if (!state_owner) {
        out.valid = false;
        out.failure_kind = LuaRuntime::ScriptFailureKind::kOther;
        out.error = "failed to create lua state";
        return out;
    }

    sol::state_view state(state_owner.get());
    open_allowed_libraries(state, policy);

    sol::environment env(state, sol::create, state.globals());
    const LuaRuntime::HostCallContext host_context{
        std::string(func_name),
        std::string(env_name),
        hook_context,
    };
    bind_host_api_to_environment(state, env, host_api, host_context);

    sol::table ctx_table = build_context_table(state, hook_context);
    env["ctx"] = ctx_table;

    auto script_exec = state.safe_script(
        script_text,
        env,
        sol::script_pass_on_error,
        std::string(chunk_name),
        sol::load_mode::text);
    out.peak_memory_bytes = budget.peak_bytes;
    if (!script_exec.valid()) {
        const sol::error err = script_exec;
        out.valid = false;
        out.failure_kind = classify_failure(budget).value_or(LuaRuntime::ScriptFailureKind::kOther);
        out.error = err.what();
        return out;
    }

    ScriptDecisionResult top_level_result{};
    if (script_exec.return_count() > 0) {
        top_level_result = parse_return_table_from_lua_object(
            script_exec.get<sol::object>(0),
            func_name);
        if (!top_level_result.valid) {
            top_level_result.peak_memory_bytes = budget.peak_bytes;
            return top_level_result;
        }
    }

    const sol::object hook_object = env[std::string(func_name)];
    if (hook_object.valid() && hook_object.get_type() == sol::type::function) {
        const sol::protected_function hook = hook_object.as<sol::protected_function>();
        auto hook_exec = hook(ctx_table);
        out.peak_memory_bytes = budget.peak_bytes;
        out.executed = true;
        if (!hook_exec.valid()) {
            const sol::error err = hook_exec;
            out.valid = false;
            out.failure_kind = classify_failure(budget).value_or(LuaRuntime::ScriptFailureKind::kOther);
            out.error = err.what();
            return out;
        }

        if (hook_exec.return_count() > 0) {
            const ScriptDecisionResult hook_result = parse_return_table_from_lua_object(
                hook_exec.get<sol::object>(0),
                func_name);
            if (!hook_result.valid) {
                ScriptDecisionResult failed = hook_result;
                failed.peak_memory_bytes = budget.peak_bytes;
                failed.executed = true;
                return failed;
            }

            out.decision = hook_result.decision;
            out.reason = hook_result.reason;
            out.notice = hook_result.notice;
            return out;
        }
    }

    if (top_level_result.valid) {
        out.decision = top_level_result.decision;
        out.reason = top_level_result.reason;
        out.notice = top_level_result.notice;
    }

    if (const auto directive = find_matching_directive(script_text, func_name);
        directive.has_value() && !directive->decision_token.empty()) {
        LuaHookDecision directive_decision = LuaHookDecision::kPass;
        if (!parse_hook_decision(directive->decision_token, directive_decision)) {
            out.valid = false;
            out.failure_kind = LuaRuntime::ScriptFailureKind::kOther;
            out.error = "invalid decision token: " + directive->decision_token;
            return out;
        }

        if (hook_decision_rank(out.decision) == hook_decision_rank(LuaHookDecision::kPass)
            && out.reason.empty() && out.notice.empty()) {
            out.decision = directive_decision;
            out.reason = directive->reason;
        }
    }

    return out;
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

    std::size_t peak_memory_bytes = 0;
    if (const auto validation_error = validate_script_with_lua(path, policy_, peak_memory_bytes);
        validation_error.has_value()) {
        memory_used_bytes_ = peak_memory_bytes;
        ++errors_total_;
        return LoadResult{false, *validation_error};
    }

    memory_used_bytes_ = peak_memory_bytes;
    loaded_scripts_[env_name] = path;
    return LoadResult{true, {}};
}

LuaRuntime::ReloadResult LuaRuntime::reload_scripts(const std::vector<ScriptEntry>& scripts) {
    std::lock_guard<std::mutex> lock(mu_);

    std::unordered_map<std::string, std::filesystem::path> reloaded;
    reloaded.reserve(scripts.size());

    std::size_t failed = 0;
    std::size_t peak_memory_bytes = 0;
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

        std::size_t script_peak_memory = 0;
        if (const auto validation_error = validate_script_with_lua(script.path, policy_, script_peak_memory);
            validation_error.has_value()) {
            peak_memory_bytes = std::max(peak_memory_bytes, script_peak_memory);
            ++errors_total_;
            ++failed;
            continue;
        }

        peak_memory_bytes = std::max(peak_memory_bytes, script_peak_memory);
        auto [it, inserted] = reloaded.emplace(script.env_name, script.path);
        if (!inserted) {
            ++errors_total_;
            ++failed;
            it->second = script.path;
        }
    }

    memory_used_bytes_ = peak_memory_bytes;
    loaded_scripts_ = std::move(reloaded);
    ++reload_epoch_;
    return ReloadResult{loaded_scripts_.size(), failed, {}};
}

LuaRuntime::CallResult LuaRuntime::call(const std::string& env_name,
                                        const std::string& func_name,
                                        const LuaHookContext& ctx) {
    std::lock_guard<std::mutex> lock(mu_);

    if (func_name.empty()) {
        ++errors_total_;
        return CallResult{false, false, "func_name is empty"};
    }

    const auto it = loaded_scripts_.find(env_name);
    if (it == loaded_scripts_.end()) {
        return CallResult{true, false, {}};
    }

    std::string read_error;
    const auto script_text = read_script_text(it->second, read_error);
    if (!script_text.has_value()) {
        ++errors_total_;
        return CallResult{false, false, read_error};
    }

    const ScriptDecisionResult result = execute_script(
        *script_text,
        it->second.string(),
        env_name,
        func_name,
        ctx,
        policy_,
        host_api_);
    memory_used_bytes_ = result.peak_memory_bytes;
    if (!result.valid) {
        switch (result.failure_kind) {
        case ScriptFailureKind::kInstructionLimit:
            ++instruction_limit_hits_;
            break;
        case ScriptFailureKind::kMemoryLimit:
            ++memory_limit_hits_;
            break;
        case ScriptFailureKind::kNone:
        case ScriptFailureKind::kOther:
        default:
            break;
        }
        ++errors_total_;
        return CallResult{false, result.executed, result.error};
    }

    ++calls_total_;
    return CallResult{true, result.executed, {}};
}

LuaRuntime::CallAllResult LuaRuntime::call_all(const std::string& func_name,
                                               const LuaHookContext& ctx) {
    std::lock_guard<std::mutex> lock(mu_);

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

    std::size_t peak_memory_bytes = 0;
    for (const auto& [env_name, path] : ordered_scripts) {
        CallAllResult::ScriptCallResult script_call{};
        script_call.env_name = env_name;

        std::string read_error;
        const auto script_text = read_script_text(path, read_error);
        if (!script_text.has_value()) {
            script_call.failed = true;
            script_call.failure_kind = ScriptFailureKind::kOther;
            script_results.push_back(std::move(script_call));
            ++failed;
            ++errors_total_;
            if (first_error.empty()) {
                first_error = read_error;
            }
            continue;
        }

        const ScriptDecisionResult script_result = execute_script(
            *script_text,
            path.string(),
            env_name,
            func_name,
            ctx,
            policy_,
            host_api_);
        peak_memory_bytes = std::max(peak_memory_bytes, script_result.peak_memory_bytes);

        if (!script_result.valid) {
            script_call.failed = true;
            script_call.failure_kind = script_result.failure_kind;
            script_results.push_back(std::move(script_call));

            switch (script_result.failure_kind) {
            case ScriptFailureKind::kInstructionLimit:
                ++instruction_limit_hits_;
                break;
            case ScriptFailureKind::kMemoryLimit:
                ++memory_limit_hits_;
                break;
            case ScriptFailureKind::kNone:
            case ScriptFailureKind::kOther:
            default:
                break;
            }

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

    memory_used_bytes_ = peak_memory_bytes;
    calls_total_ += attempted;
    return CallAllResult{
        attempted,
        failed,
        aggregated_decision,
        aggregated_reason,
        std::move(aggregated_notices),
        first_error,
        std::move(script_results),
    };
}

bool LuaRuntime::register_host_api(const std::string& table_name,
                                   const std::string& func_name,
                                   HostCallback callback) {
    std::lock_guard<std::mutex> lock(mu_);

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
    memory_used_bytes_ = 0;
    calls_total_ = 0;
    errors_total_ = 0;
    instruction_limit_hits_ = 0;
    memory_limit_hits_ = 0;
    reload_epoch_ = 0;
}

bool LuaRuntime::enabled() const {
    return true;
}

LuaRuntime::MetricsSnapshot LuaRuntime::metrics_snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);

    MetricsSnapshot snapshot{};
    snapshot.loaded_scripts = loaded_scripts_.size();
    snapshot.registered_host_api = host_api_.size();
    snapshot.memory_used_bytes = memory_used_bytes_;
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
