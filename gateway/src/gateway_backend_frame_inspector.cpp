#include "gateway_backend_frame_inspector.hpp"

#include "server/core/protocol/packet.hpp"
#include "server/protocol/game_opcodes.hpp"

#include <cstddef>

namespace core_proto = server::core::protocol;
namespace game_proto = server::protocol;

namespace gateway {

std::vector<std::uint8_t> inspect_backend_frames(
    std::vector<std::uint8_t>& prebuffer,
    std::span<const std::uint8_t> payload,
    const std::function<void(std::span<const std::uint8_t>)>& on_login_res,
    const std::function<bool(std::uint16_t, std::span<const std::uint8_t>)>& on_frame) {
    if (payload.empty()) {
        return {};
    }

    constexpr std::size_t kMaxInspectableBytes = 256 * 1024;
    if (prebuffer.size() + payload.size() > kMaxInspectableBytes) {
        prebuffer.clear();
    }

    prebuffer.insert(prebuffer.end(), payload.begin(), payload.end());

    std::vector<std::uint8_t> passthrough_payload;
    while (prebuffer.size() >= core_proto::k_header_bytes) {
        core_proto::PacketHeader header{};
        core_proto::decode_header(prebuffer.data(), header);
        const std::size_t frame_bytes =
            core_proto::k_header_bytes + static_cast<std::size_t>(header.length);
        if (prebuffer.size() < frame_bytes) {
            break;
        }

        const auto frame_span = std::span<const std::uint8_t>(prebuffer.data(), frame_bytes);
        const auto body_span = std::span<const std::uint8_t>(
            prebuffer.data() + core_proto::k_header_bytes,
            static_cast<std::size_t>(header.length));

        if (header.msg_id == game_proto::MSG_LOGIN_RES && on_login_res) {
            on_login_res(body_span);
        }

        const bool consumed =
            on_frame ? on_frame(header.msg_id, frame_span) : false;
        if (!consumed) {
            passthrough_payload.insert(
                passthrough_payload.end(), frame_span.begin(), frame_span.end());
        }

        prebuffer.erase(
            prebuffer.begin(),
            prebuffer.begin() + static_cast<std::ptrdiff_t>(frame_bytes));
    }

    return passthrough_payload;
}

} // namespace gateway
