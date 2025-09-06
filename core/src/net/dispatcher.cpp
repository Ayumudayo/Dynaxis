#include "server/core/dispatcher.hpp"
#include "server/core/session.hpp"

namespace server::core {

void Dispatcher::register_handler(std::uint16_t msg_id, handler_t handler) {
    table_[msg_id] = std::move(handler);
}

bool Dispatcher::dispatch(std::uint16_t msg_id, Session& s, std::span<const std::uint8_t> payload) const {
    auto it = table_.find(msg_id);
    if (it == table_.end()) return false;
    it->second(s, payload);
    return true;
}

} // namespace server::core

