#include "server/scripting/chat_lua_bindings.hpp"

#include "server/core/scripting/lua_runtime.hpp"
#include "server/core/util/log.hpp"

#include <limits>

namespace server::app::scripting {

namespace corelog = server::core::log;

namespace {

using HostArgs = server::core::scripting::LuaRuntime::HostArgs;
using HostCallContext = server::core::scripting::LuaRuntime::HostCallContext;
using HostCallResult = server::core::scripting::LuaRuntime::HostCallResult;
using HostValue = server::core::scripting::LuaRuntime::HostValue;

HostCallResult make_error(std::string message) {
    HostCallResult result{};
    result.error = std::move(message);
    return result;
}

HostCallResult make_success(HostValue value = {}) {
    HostCallResult result{};
    result.value = std::move(value);
    return result;
}

std::optional<std::uint32_t> get_u32_arg(const HostArgs& args,
                                         std::size_t index,
                                         const char* name,
                                         HostCallResult& error_result) {
    if (index >= args.size()) {
        error_result = make_error(std::string(name) + " argument is missing");
        return std::nullopt;
    }

    const auto* value = std::get_if<std::int64_t>(&args[index].value);
    if (value == nullptr || *value < 0 || *value > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
        error_result = make_error(std::string(name) + " argument must be a non-negative integer");
        return std::nullopt;
    }

    return static_cast<std::uint32_t>(*value);
}

std::optional<std::string> get_string_arg(const HostArgs& args,
                                          std::size_t index,
                                          const char* name,
                                          HostCallResult& error_result) {
    if (index >= args.size()) {
        error_result = make_error(std::string(name) + " argument is missing");
        return std::nullopt;
    }

    const auto* value = std::get_if<std::string>(&args[index].value);
    if (value == nullptr) {
        error_result = make_error(std::string(name) + " argument must be a string");
        return std::nullopt;
    }
    return *value;
}

void log_with_context(std::string_view level,
                      std::string_view message,
                      const HostCallContext& context) {
    std::string formatted = "[lua]";
    if (!context.hook_name.empty()) {
        formatted += " hook=" + context.hook_name;
    }
    if (!context.script_name.empty()) {
        formatted += " script=" + context.script_name;
    }
    formatted += " ";
    formatted += message;

    if (level == "info") {
        corelog::info(formatted);
    } else if (level == "warn") {
        corelog::warn(formatted);
    } else {
        corelog::debug(formatted);
    }
}

} // namespace

std::size_t chat_lua_binding_count() {
    return 20;
}

ChatLuaBindingsResult register_chat_lua_bindings(server::core::scripting::LuaRuntime& runtime,
                                                 ChatLuaHost& host) {
    ChatLuaBindingsResult result{};

    const auto register_binding = [&](const char* table,
                                      const char* name,
                                      server::core::scripting::LuaRuntime::HostCallback callback) {
        ++result.attempted;
        if (runtime.register_host_api(table, name, std::move(callback))) {
            ++result.registered;
        }
    };

    register_binding("server", "get_user_name", [&host](const HostArgs& args, const HostCallContext& context) {
        HostCallResult error_result{};
        const auto session_id = get_u32_arg(args, 0, "session_id", error_result);
        if (!session_id.has_value()) {
            return error_result;
        }
        if (const auto user = host.lua_get_user_name(*session_id); user.has_value()) {
            return make_success(HostValue{*user});
        }
        if (context.hook.session_id == *session_id && !context.hook.user.empty()) {
            return make_success(HostValue{context.hook.user});
        }
        return make_success();
    });

    register_binding("server", "get_user_room", [&host](const HostArgs& args, const HostCallContext& context) {
        HostCallResult error_result{};
        const auto session_id = get_u32_arg(args, 0, "session_id", error_result);
        if (!session_id.has_value()) {
            return error_result;
        }
        if (const auto room = host.lua_get_user_room(*session_id); room.has_value()) {
            return make_success(HostValue{*room});
        }
        if (context.hook.session_id == *session_id && !context.hook.room.empty()) {
            return make_success(HostValue{context.hook.room});
        }
        return make_success();
    });

    register_binding("server", "get_room_users", [&host](const HostArgs& args, const HostCallContext&) {
        HostCallResult error_result{};
        const auto room_name = get_string_arg(args, 0, "room_name", error_result);
        if (!room_name.has_value()) {
            return error_result;
        }
        return make_success(HostValue{host.lua_get_room_users(*room_name)});
    });

    register_binding("server", "get_room_list", [&host](const HostArgs&, const HostCallContext&) {
        return make_success(HostValue{host.lua_get_room_list()});
    });

    register_binding("server", "get_room_owner", [&host](const HostArgs& args, const HostCallContext&) {
        HostCallResult error_result{};
        const auto room_name = get_string_arg(args, 0, "room_name", error_result);
        if (!room_name.has_value()) {
            return error_result;
        }
        if (const auto owner = host.lua_get_room_owner(*room_name); owner.has_value()) {
            return make_success(HostValue{*owner});
        }
        return make_success();
    });

    register_binding("server", "is_user_muted", [&host](const HostArgs& args, const HostCallContext&) {
        HostCallResult error_result{};
        const auto nickname = get_string_arg(args, 0, "nickname", error_result);
        if (!nickname.has_value()) {
            return error_result;
        }
        return make_success(HostValue{host.lua_is_user_muted(*nickname)});
    });

    register_binding("server", "is_user_banned", [&host](const HostArgs& args, const HostCallContext&) {
        HostCallResult error_result{};
        const auto nickname = get_string_arg(args, 0, "nickname", error_result);
        if (!nickname.has_value()) {
            return error_result;
        }
        return make_success(HostValue{host.lua_is_user_banned(*nickname)});
    });

    register_binding("server", "get_online_count", [&host](const HostArgs&, const HostCallContext&) {
        return make_success(HostValue{static_cast<std::int64_t>(host.lua_get_online_count())});
    });

    register_binding("server", "get_room_count", [&host](const HostArgs&, const HostCallContext&) {
        return make_success(HostValue{static_cast<std::int64_t>(host.lua_get_room_count())});
    });

    register_binding("server", "send_notice", [&host](const HostArgs& args, const HostCallContext&) {
        HostCallResult error_result{};
        const auto session_id = get_u32_arg(args, 0, "session_id", error_result);
        if (!session_id.has_value()) {
            return error_result;
        }
        const auto text = get_string_arg(args, 1, "text", error_result);
        if (!text.has_value()) {
            return error_result;
        }
        if (!host.lua_send_notice(*session_id, *text)) {
            return make_error("failed to queue send_notice");
        }
        return make_success();
    });

    register_binding("server", "broadcast_room", [&host](const HostArgs& args, const HostCallContext&) {
        HostCallResult error_result{};
        const auto room_name = get_string_arg(args, 0, "room_name", error_result);
        if (!room_name.has_value()) {
            return error_result;
        }
        const auto text = get_string_arg(args, 1, "text", error_result);
        if (!text.has_value()) {
            return error_result;
        }
        if (!host.lua_broadcast_room(*room_name, *text)) {
            return make_error("failed to queue broadcast_room");
        }
        return make_success();
    });

    register_binding("server", "broadcast_all", [&host](const HostArgs& args, const HostCallContext&) {
        HostCallResult error_result{};
        const auto text = get_string_arg(args, 0, "text", error_result);
        if (!text.has_value()) {
            return error_result;
        }
        if (!host.lua_broadcast_all(*text)) {
            return make_error("failed to queue broadcast_all");
        }
        return make_success();
    });

    register_binding("server", "kick_user", [&host](const HostArgs& args, const HostCallContext&) {
        HostCallResult error_result{};
        const auto session_id = get_u32_arg(args, 0, "session_id", error_result);
        if (!session_id.has_value()) {
            return error_result;
        }
        const auto reason = get_string_arg(args, 1, "reason", error_result);
        if (!reason.has_value()) {
            return error_result;
        }
        if (!host.lua_kick_user(*session_id, *reason)) {
            return make_error("failed to queue kick_user");
        }
        return make_success();
    });

    register_binding("server", "mute_user", [&host](const HostArgs& args, const HostCallContext&) {
        HostCallResult error_result{};
        const auto nickname = get_string_arg(args, 0, "nickname", error_result);
        if (!nickname.has_value()) {
            return error_result;
        }
        const auto duration_sec = get_u32_arg(args, 1, "duration_sec", error_result);
        if (!duration_sec.has_value()) {
            return error_result;
        }
        const auto reason = get_string_arg(args, 2, "reason", error_result);
        if (!reason.has_value()) {
            return error_result;
        }
        if (!host.lua_mute_user(*nickname, *duration_sec, *reason)) {
            return make_error("failed to queue mute_user");
        }
        return make_success();
    });

    register_binding("server", "ban_user", [&host](const HostArgs& args, const HostCallContext&) {
        HostCallResult error_result{};
        const auto nickname = get_string_arg(args, 0, "nickname", error_result);
        if (!nickname.has_value()) {
            return error_result;
        }
        const auto duration_sec = get_u32_arg(args, 1, "duration_sec", error_result);
        if (!duration_sec.has_value()) {
            return error_result;
        }
        const auto reason = get_string_arg(args, 2, "reason", error_result);
        if (!reason.has_value()) {
            return error_result;
        }
        if (!host.lua_ban_user(*nickname, *duration_sec, *reason)) {
            return make_error("failed to queue ban_user");
        }
        return make_success();
    });

    register_binding("server", "log_info", [](const HostArgs& args, const HostCallContext& context) {
        HostCallResult error_result{};
        const auto message = get_string_arg(args, 0, "message", error_result);
        if (!message.has_value()) {
            return error_result;
        }
        log_with_context("info", *message, context);
        return make_success();
    });

    register_binding("server", "log_warn", [](const HostArgs& args, const HostCallContext& context) {
        HostCallResult error_result{};
        const auto message = get_string_arg(args, 0, "message", error_result);
        if (!message.has_value()) {
            return error_result;
        }
        log_with_context("warn", *message, context);
        return make_success();
    });

    register_binding("server", "log_debug", [](const HostArgs& args, const HostCallContext& context) {
        HostCallResult error_result{};
        const auto message = get_string_arg(args, 0, "message", error_result);
        if (!message.has_value()) {
            return error_result;
        }
        log_with_context("debug", *message, context);
        return make_success();
    });

    register_binding("server", "hook_name", [](const HostArgs&, const HostCallContext& context) {
        return make_success(HostValue{context.hook_name});
    });

    register_binding("server", "script_name", [](const HostArgs&, const HostCallContext& context) {
        return make_success(HostValue{context.script_name});
    });

    return result;
}

} // namespace server::app::scripting
