#include <type_traits>

#include <server/chat/chat_service.hpp>

int main() {
    using ChatService = server::app::chat::ChatService;

    static_assert(std::is_class_v<ChatService>);

    ChatService* service = nullptr;
    (void)service;
    return 0;
}
