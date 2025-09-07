#include "server/core/dispatcher.hpp"
#include "server/core/session.hpp"
#include "server/core/util/log.hpp"
#include "server/core/protocol_errors.hpp"

namespace server::core {

void Dispatcher::register_handler(std::uint16_t msg_id, handler_t handler) {
    table_[msg_id] = std::move(handler);
}

bool Dispatcher::dispatch(std::uint16_t msg_id, Session& s, std::span<const std::uint8_t> payload) const {
    auto it = table_.find(msg_id);
    if (it == table_.end()) return false;
    try {
        it->second(s, payload);
    } catch (const std::exception& ex) {
        server::core::log::error(std::string("handler exception for msg=") + std::to_string(msg_id) + ": " + ex.what());
        try { s.send_error(server::core::protocol::errc::INTERNAL_ERROR, "internal error"); } catch (...) {}
    } catch (...) {
        server::core::log::error(std::string("handler unknown exception for msg=") + std::to_string(msg_id));
        try { s.send_error(server::core::protocol::errc::INTERNAL_ERROR, "internal error"); } catch (...) {}
    }
    return true;
}

} // namespace server::core

