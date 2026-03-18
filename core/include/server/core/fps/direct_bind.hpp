#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "server/core/protocol/packet.hpp"

namespace server::core::fps {

/** @brief Direct UDP bind request payload contract. */
struct DirectBindRequest {
    std::string session_id;
    std::uint64_t nonce{0};
    std::uint64_t expires_unix_ms{0};
    std::string token;
};

/** @brief Direct UDP bind ticket contract. */
struct DirectBindTicket {
    std::string session_id;
    std::uint64_t nonce{0};
    std::uint64_t expires_unix_ms{0};
    std::string token;
};

/** @brief Direct UDP bind response payload contract. */
struct DirectBindResponse {
    std::uint16_t code{0};
    DirectBindTicket ticket;
    std::string message;
};

inline void write_be64(std::uint64_t value, std::vector<std::uint8_t>& out) {
    out.push_back(static_cast<std::uint8_t>((value >> 56) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 48) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 40) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 32) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
}

inline bool read_be64(std::span<const std::uint8_t>& in, std::uint64_t& out) {
    if (in.size() < 8) {
        return false;
    }

    out = (static_cast<std::uint64_t>(in[0]) << 56)
        | (static_cast<std::uint64_t>(in[1]) << 48)
        | (static_cast<std::uint64_t>(in[2]) << 40)
        | (static_cast<std::uint64_t>(in[3]) << 32)
        | (static_cast<std::uint64_t>(in[4]) << 24)
        | (static_cast<std::uint64_t>(in[5]) << 16)
        | (static_cast<std::uint64_t>(in[6]) << 8)
        | static_cast<std::uint64_t>(in[7]);
    in = in.subspan(8);
    return true;
}

inline std::vector<std::uint8_t> encode_direct_bind_request_payload(const DirectBindRequest& request) {
    std::vector<std::uint8_t> payload;
    payload.reserve(2 + request.session_id.size() + 8 + 8 + 2 + request.token.size());
    server::core::protocol::write_lp_utf8(payload, request.session_id);
    write_be64(request.nonce, payload);
    write_be64(request.expires_unix_ms, payload);
    server::core::protocol::write_lp_utf8(payload, request.token);
    return payload;
}

inline bool decode_direct_bind_request_payload(std::span<const std::uint8_t> payload, DirectBindRequest& out) {
    auto in = payload;
    if (!server::core::protocol::read_lp_utf8(in, out.session_id)) {
        return false;
    }
    if (!read_be64(in, out.nonce)) {
        return false;
    }
    if (!read_be64(in, out.expires_unix_ms)) {
        return false;
    }
    if (!server::core::protocol::read_lp_utf8(in, out.token)) {
        return false;
    }
    return in.empty();
}

inline std::vector<std::uint8_t> encode_direct_bind_response_payload(std::uint16_t code,
                                                                     const DirectBindTicket& ticket,
                                                                     std::string_view message) {
    std::vector<std::uint8_t> payload;
    payload.reserve(2 + 2 + ticket.session_id.size() + 8 + 8 + 2 + ticket.token.size() + 2 + message.size());

    std::array<std::uint8_t, 2> code_buf{};
    server::core::protocol::write_be16(code, code_buf.data());
    payload.insert(payload.end(), code_buf.begin(), code_buf.end());

    server::core::protocol::write_lp_utf8(payload, ticket.session_id);
    write_be64(ticket.nonce, payload);
    write_be64(ticket.expires_unix_ms, payload);
    server::core::protocol::write_lp_utf8(payload, ticket.token);
    server::core::protocol::write_lp_utf8(payload, message);
    return payload;
}

inline bool decode_direct_bind_response_payload(std::span<const std::uint8_t> payload, DirectBindResponse& out) {
    auto in = payload;
    if (in.size() < 2) {
        return false;
    }
    out.code = server::core::protocol::read_be16(in.data());
    in = in.subspan(2);
    if (!server::core::protocol::read_lp_utf8(in, out.ticket.session_id)) {
        return false;
    }
    if (!read_be64(in, out.ticket.nonce)) {
        return false;
    }
    if (!read_be64(in, out.ticket.expires_unix_ms)) {
        return false;
    }
    if (!server::core::protocol::read_lp_utf8(in, out.ticket.token)) {
        return false;
    }
    if (!server::core::protocol::read_lp_utf8(in, out.message)) {
        return false;
    }
    return in.empty();
}

} // namespace server::core::fps
