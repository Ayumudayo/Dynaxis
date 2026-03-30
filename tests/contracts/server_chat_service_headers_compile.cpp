#include <type_traits>

#include <server/chat/chat_service.hpp>

int main() {
    using ChatService = server::app::chat::ChatService;
    using ChatLuaHost = server::app::scripting::ChatLuaHost;

    static_assert(std::is_class_v<ChatService>);
    static_assert(!std::is_convertible_v<ChatService*, ChatLuaHost*>);

    ChatService* service = nullptr;
    (void)service;
    return 0;
}
