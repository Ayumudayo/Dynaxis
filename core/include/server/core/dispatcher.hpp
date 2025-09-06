#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>
#include <span>

namespace server::core {

class Session;

class Dispatcher {
public:
    using handler_t = std::function<void(Session&, std::span<const std::uint8_t>)>;

    void register_handler(std::uint16_t msg_id, handler_t handler);
    bool dispatch(std::uint16_t msg_id, Session& s, std::span<const std::uint8_t> payload) const;

private:
    std::unordered_map<std::uint16_t, handler_t> table_;
};

} // namespace server::core

