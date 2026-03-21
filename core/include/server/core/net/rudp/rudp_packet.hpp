#pragma once

#include <cstddef>
#include <cstring>
#include <cstdint>
#include <span>
#include <vector>

#include "server/core/protocol/packet.hpp"

namespace server::core::net::rudp {

/** @brief RUDP 패킷 매직 값("RU")입니다. 빠른 프로토콜 식별에 사용합니다. */
inline constexpr std::uint16_t kRudpMagic = 0x5255;
/** @brief RUDP 헤더 직렬화 길이(byte)입니다. */
inline constexpr std::size_t kRudpHeaderBytes = 34;

/** @brief RUDP 제어 패킷 타입입니다. HELLO/ACK/DATA/CLOSE 같은 상태 전이 종류를 구분합니다. */
enum class PacketType : std::uint8_t {
    kHello = 1,
    kHelloAck = 2,
    kData = 3,
    kPing = 4,
    kClose = 5,
};

/** @brief RUDP flags 비트 정의입니다. ACK 전용 전송과 재전송 여부를 구분합니다. */
enum PacketFlags : std::uint8_t {
    kFlagAckOnly = 1 << 0,
    kFlagRetransmit = 1 << 1,
};

/** @brief RUDP outer header입니다. inner payload 앞에 붙는 공용 wire header입니다. */
struct RudpHeader {
    std::uint16_t magic{kRudpMagic};
    std::uint8_t version{1};
    PacketType type{PacketType::kData};
    std::uint32_t connection_id{0};
    std::uint32_t packet_number{0};
    std::uint32_t ack_largest{0};
    std::uint64_t ack_mask{0};
    std::uint16_t ack_delay_ms{0};
    std::uint8_t channel{0};
    std::uint8_t flags{0};
    std::uint32_t timestamp_ms{0};
    std::uint16_t payload_length{0};
};

/** @brief RUDP decode 결과입니다. 헤더와 payload span을 함께 반환합니다. */
struct DecodeResult {
    bool ok{false};
    RudpHeader header{};
    std::span<const std::uint8_t> payload{};
};

/** @brief RUDP 헤더를 wire bytes로 직렬화합니다. */
inline void encode_header(const RudpHeader& header, std::uint8_t* out) {
    server::core::protocol::write_be16(header.magic, out + 0);
    out[2] = header.version;
    out[3] = static_cast<std::uint8_t>(header.type);
    server::core::protocol::write_be32(header.connection_id, out + 4);
    server::core::protocol::write_be32(header.packet_number, out + 8);
    server::core::protocol::write_be32(header.ack_largest, out + 12);
    server::core::protocol::write_be32(static_cast<std::uint32_t>((header.ack_mask >> 32) & 0xFFFFFFFFu), out + 16);
    server::core::protocol::write_be32(static_cast<std::uint32_t>(header.ack_mask & 0xFFFFFFFFu), out + 20);
    server::core::protocol::write_be16(header.ack_delay_ms, out + 24);
    out[26] = header.channel;
    out[27] = header.flags;
    server::core::protocol::write_be32(header.timestamp_ms, out + 28);
    server::core::protocol::write_be16(header.payload_length, out + 32);
}

/** @brief wire bytes를 RUDP 헤더와 payload로 역직렬화합니다. */
inline DecodeResult decode_packet(std::span<const std::uint8_t> datagram) {
    DecodeResult result{};
    if (datagram.size() < kRudpHeaderBytes) {
        return result;
    }

    RudpHeader header{};
    header.magic = server::core::protocol::read_be16(datagram.data() + 0);
    header.version = datagram[2];
    header.type = static_cast<PacketType>(datagram[3]);
    header.connection_id = server::core::protocol::read_be32(datagram.data() + 4);
    header.packet_number = server::core::protocol::read_be32(datagram.data() + 8);
    header.ack_largest = server::core::protocol::read_be32(datagram.data() + 12);
    const auto ack_hi = static_cast<std::uint64_t>(server::core::protocol::read_be32(datagram.data() + 16));
    const auto ack_lo = static_cast<std::uint64_t>(server::core::protocol::read_be32(datagram.data() + 20));
    header.ack_mask = (ack_hi << 32) | ack_lo;
    header.ack_delay_ms = server::core::protocol::read_be16(datagram.data() + 24);
    header.channel = datagram[26];
    header.flags = datagram[27];
    header.timestamp_ms = server::core::protocol::read_be32(datagram.data() + 28);
    header.payload_length = server::core::protocol::read_be16(datagram.data() + 32);

    if (datagram.size() != (kRudpHeaderBytes + header.payload_length)) {
        return result;
    }

    result.ok = true;
    result.header = header;
    result.payload = datagram.subspan(kRudpHeaderBytes, header.payload_length);
    return result;
}

/** @brief 헤더와 payload를 단일 datagram으로 만듭니다. */
inline std::vector<std::uint8_t> encode_packet(const RudpHeader& header, std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> out(kRudpHeaderBytes + payload.size());
    RudpHeader mutable_header = header;
    mutable_header.payload_length = static_cast<std::uint16_t>(payload.size());
    encode_header(mutable_header, out.data());
    if (!payload.empty()) {
        std::memcpy(out.data() + kRudpHeaderBytes, payload.data(), payload.size());
    }
    return out;
}

/** @brief datagram이 RUDP magic을 가지는지 빠르게 검사합니다. fast-path 분기에서 decode 비용을 줄일 때 사용합니다. */
inline bool looks_like_rudp(std::span<const std::uint8_t> datagram) {
    return datagram.size() >= 2 && server::core::protocol::read_be16(datagram.data()) == kRudpMagic;
}

} // namespace server::core::net::rudp
