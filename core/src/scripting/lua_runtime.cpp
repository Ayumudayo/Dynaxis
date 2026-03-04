#include "server/core/scripting/lua_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>

namespace server::core::scripting {

namespace {

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

std::optional<std::string> extract_quoted_value_after_key(std::string_view text,
                                                          std::string_view key) {
    std::size_t key_pos = text.find(key);
    while (key_pos != std::string_view::npos) {
        const bool left_boundary = key_pos == 0
            || (std::isalnum(static_cast<unsigned char>(text[key_pos - 1])) == 0
                && text[key_pos - 1] != '_');
        const std::size_t right_index = key_pos + key.size();
        const bool right_boundary = right_index >= text.size()
            || (std::isalnum(static_cast<unsigned char>(text[right_index])) == 0
                && text[right_index] != '_');

        if (left_boundary && right_boundary) {
            break;
        }

        key_pos = text.find(key, key_pos + 1);
    }

    if (key_pos == std::string_view::npos) {
        return std::nullopt;
    }

    const std::size_t eq_pos = text.find('=', key_pos + key.size());
    if (eq_pos == std::string_view::npos) {
        return std::nullopt;
    }

    std::size_t value_pos = eq_pos + 1;
    while (value_pos < text.size()
           && std::isspace(static_cast<unsigned char>(text[value_pos])) != 0) {
        ++value_pos;
    }

    if (value_pos >= text.size()) {
        return std::nullopt;
    }

    const char quote = text[value_pos];
    if (quote != '\'' && quote != '"') {
        return std::nullopt;
    }

    ++value_pos;
    const std::size_t end_pos = text.find(quote, value_pos);
    if (end_pos == std::string_view::npos || end_pos < value_pos) {
        return std::nullopt;
    }

    return std::string(text.substr(value_pos, end_pos - value_pos));
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
    std::string reason;
};

ParsedDirective parse_directive_line(std::string_view line) {
    ParsedDirective out{};

    const std::string trimmed = trim_ascii(line);
    if (trimmed.rfind("--", 0) != 0) {
        return out;
    }

    const std::string payload = trim_ascii(std::string_view(trimmed).substr(2));
    if (payload.rfind("decision=", 0) != 0 && payload.rfind("hook=", 0) != 0) {
        return out;
    }

    const auto decision_token = extract_word_value(payload, "decision=");
    if (!decision_token.has_value() || decision_token->empty()) {
        return out;
    }

    out.present = true;
    out.decision_token = to_lower_ascii(trim_ascii(*decision_token));

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
    std::string reason;
    std::string error;
};

ScriptDecisionResult parse_return_table_scaffold(std::string_view script_text,
                                                 std::string_view func_name) {
    ScriptDecisionResult out{};

    if (script_text.find("return") == std::string_view::npos
        || script_text.find('{') == std::string_view::npos
        || script_text.find('}') == std::string_view::npos) {
        return out;
    }

    const auto decision_token = extract_quoted_value_after_key(script_text, "decision");
    if (!decision_token.has_value() || decision_token->empty()) {
        return out;
    }

    if (!parse_hook_decision(*decision_token, out.decision)) {
        out.valid = false;
        out.error = "invalid decision token in return table: " + *decision_token;
        return out;
    }

    if (const auto hook_token = extract_quoted_value_after_key(script_text, "hook");
        hook_token.has_value() && !hook_token->empty()) {
        const std::string token = to_lower_ascii(trim_ascii(*hook_token));
        const std::string target = to_lower_ascii(std::string(func_name));
        if (token != target) {
            return ScriptDecisionResult{};
        }
    }

    if (const auto reason_token = extract_quoted_value_after_key(script_text, "reason");
        reason_token.has_value()) {
        out.reason = *reason_token;
    }

    return out;
}

ScriptDecisionResult read_script_scaffold_decision(
    const std::filesystem::path& path,
    std::string_view func_name) {
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

        LuaHookDecision decision = LuaHookDecision::kPass;
        if (!parse_hook_decision(directive.decision_token, decision)) {
            ScriptDecisionResult out{};
            out.valid = false;
            out.error = "invalid decision token: " + directive.decision_token + " path=" + path.string();
            return out;
        }

        ScriptDecisionResult out{};
        out.decision = decision;
        out.reason = directive.reason;
        return out;
    }

    ScriptDecisionResult table_result = parse_return_table_scaffold(script_text, func_name);
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

        auto [it, inserted] = reloaded.emplace(script.env_name, script.path);
        if (!inserted) {
            ++errors_total_;
            ++failed;
            it->second = script.path;
        }
    }

    loaded_scripts_ = std::move(reloaded);
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

    (void)it;
    ++calls_total_;
    return CallResult{true, false, {}};
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
            make_disabled_error(),
        };
    }

    if (func_name.empty()) {
        ++errors_total_;
        return CallAllResult{0, 0, LuaHookDecision::kPass, {}, "func_name is empty"};
    }

    const std::size_t attempted = loaded_scripts_.size();
    std::size_t failed = 0;
    LuaHookDecision aggregated_decision = LuaHookDecision::kPass;
    std::string aggregated_reason;
    std::string first_error;

    std::vector<std::pair<std::string, std::filesystem::path>> ordered_scripts;
    ordered_scripts.reserve(loaded_scripts_.size());
    for (const auto& entry : loaded_scripts_) {
        ordered_scripts.emplace_back(entry.first, entry.second);
    }
    std::sort(ordered_scripts.begin(), ordered_scripts.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.first < rhs.first;
    });

    for (const auto& [env_name, path] : ordered_scripts) {
        (void)env_name;
        const auto script_result = read_script_scaffold_decision(path, func_name);
        if (!script_result.valid) {
            ++failed;
            ++errors_total_;
            if (first_error.empty()) {
                first_error = script_result.error;
            }
            continue;
        }

        const int current_rank = hook_decision_rank(aggregated_decision);
        const int candidate_rank = hook_decision_rank(script_result.decision);
        if (candidate_rank > current_rank) {
            aggregated_decision = script_result.decision;
            aggregated_reason = script_result.reason;
        }
    }

    calls_total_ += attempted;
    return CallAllResult{attempted, failed, aggregated_decision, aggregated_reason, first_error};
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
