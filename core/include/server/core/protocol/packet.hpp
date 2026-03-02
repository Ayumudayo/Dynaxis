#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace server::core::protocol {

// ==============================================================================
// 프로토콜 패킷 정의
// 
// 모든 메시지는 [헤더(14바이트)] + [바디(가변 길이)] 형태로 전송됩니다.
// 네트워크 전송 시 바이트 순서는 Big-Endian (Network Byte Order)을 따릅니다.
// ==============================================================================

// 프로토콜 v1.1 고정 헤더 길이(바이트)
inline constexpr std::size_t k_header_bytes = 14;

enum class ConnectionType : std::uint8_t {
    kUnknown = 0,
    kClientEdge = 1,
    kBackendBridge = 2,
};

enum class SegmentType : std::uint8_t {
    kUnknown = 0,
    kSystemControl = 1,
    kApplicationPayload = 2,
};

inline constexpr bool is_system_msg_id(std::uint16_t msg_id) noexcept {
    return msg_id >= 0x0001 && msg_id <= 0x000F;
}

inline constexpr SegmentType classify_segment_type(std::uint16_t msg_id) noexcept {
    return is_system_msg_id(msg_id) ? SegmentType::kSystemControl : SegmentType::kApplicationPayload;
}

/**
 * @brief 메시지 헤더 구조체
 */
struct PacketHeader {
    std::uint16_t length{0};      // 바디 길이 (헤더 제외)
    std::uint16_t msg_id{0};      // 메시지 타입 ID (Opcode)
    std::uint16_t flags{0};       // 플래그 (예: 압축 여부, 암호화 여부 등)
    std::uint32_t seq{0};         // 시퀀스 번호 (패킷 순서 보장 및 중복 방지)
    std::uint32_t utc_ts_ms32{0}; // 타임스탬프 (밀리초 단위, 하위 32비트)
};

// ==============================================================================
// Big-Endian 인코딩/디코딩 헬퍼
// 
// x86/x64 CPU는 Little-Endian을 사용하지만, 네트워크 표준은 Big-Endian입니다.
// 따라서 데이터를 전송하기 전에 변환(Encoding)하고, 받을 때 다시 변환(Decoding)해야 합니다.
// ==============================================================================

inline std::uint16_t read_be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

inline std::uint32_t read_be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           (static_cast<std::uint32_t>(p[3]));
}

inline void write_be16(std::uint16_t v, std::uint8_t* out) {
    out[0] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    out[1] = static_cast<std::uint8_t>(v & 0xFF);
}

inline void write_be32(std::uint32_t v, std::uint8_t* out) {
    out[0] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
    out[1] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    out[2] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    out[3] = static_cast<std::uint8_t>(v & 0xFF);
}

// 헤더 구조체를 바이트 배열로 직렬화 (Serialize)
inline void encode_header(const PacketHeader& h, std::uint8_t* out14) {
    write_be16(h.length, out14 + 0);
    write_be16(h.msg_id, out14 + 2);
    write_be16(h.flags, out14 + 4);
    write_be32(h.seq, out14 + 6);
    write_be32(h.utc_ts_ms32, out14 + 10);
}

// 바이트 배열을 헤더 구조체로 역직렬화 (Deserialize)
inline void decode_header(const std::uint8_t* in14, PacketHeader& h) {
    h.length = read_be16(in14 + 0);
    h.msg_id = read_be16(in14 + 2);
    h.flags = read_be16(in14 + 4);
    h.seq = read_be32(in14 + 6);
    h.utc_ts_ms32 = read_be32(in14 + 10);
}

// UTF-8 엄격 검증: overlong, surrogate, 범위 초과 codepoint를 거부한다.
inline bool is_valid_utf8(std::span<const std::uint8_t> s) {
    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        const std::uint8_t byte0 = s[i];
        if (byte0 < 0x80) {
            ++i;
            continue;
        }

        std::uint32_t codepoint = 0;
        std::size_t follow = 0;
        std::uint32_t min_codepoint = 0;

        if ((byte0 & 0xE0) == 0xC0) {
            follow = 1;
            codepoint = static_cast<std::uint32_t>(byte0 & 0x1F);
            min_codepoint = 0x80;
        } else if ((byte0 & 0xF0) == 0xE0) {
            follow = 2;
            codepoint = static_cast<std::uint32_t>(byte0 & 0x0F);
            min_codepoint = 0x800;
        } else if ((byte0 & 0xF8) == 0xF0) {
            follow = 3;
            codepoint = static_cast<std::uint32_t>(byte0 & 0x07);
            min_codepoint = 0x10000;
        } else {
            return false;
        }

        if (i + follow >= n) {
            return false;
        }

        for (std::size_t k = 1; k <= follow; ++k) {
            const std::uint8_t cont = s[i + k];
            if ((cont & 0xC0) != 0x80) {
                return false;
            }
            codepoint = (codepoint << 6) | static_cast<std::uint32_t>(cont & 0x3F);
        }

        if (codepoint < min_codepoint) {
            return false;
        }
        if (codepoint > 0x10FFFF) {
            return false;
        }
        if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
            return false;
        }

        i += follow + 1;
    }
    return true;
}

// length-prefixed UTF-8 문자열을 out 벡터 끝에 인코딩한다.
// [길이(2바이트)] + [문자열 바이트]
inline void write_lp_utf8(std::vector<std::uint8_t>& out, std::string_view str) {
    if (str.size() > 0xFFFF) {
        str = str.substr(0, 0xFFFF);
    }
    const auto len = static_cast<std::uint16_t>(str.size());
    const auto offset = out.size();
    out.resize(offset + 2 + len);
    write_be16(len, out.data() + offset);
    if (len != 0) {
        std::memcpy(out.data() + offset + 2, str.data(), len);
    }
}

// length-prefixed UTF-8 문자열을 읽고 out 에 저장한다. 실패하면 false.
inline bool read_lp_utf8(std::span<const std::uint8_t>& in, std::string& out) {
    if (in.size() < 2) {
        return false;
    }
    const std::uint16_t len = read_be16(in.data());
    if (in.size() < 2 + len) {
        return false;
    }
    auto payload = std::span<const std::uint8_t>(in.data() + 2, len);
    if (!is_valid_utf8(payload)) {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(payload.data()), payload.size());
    in = in.subspan(2 + len);
    return true;
}

} // namespace server::core::protocol
