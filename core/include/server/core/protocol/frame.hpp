#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <span>
#include <cstring>

namespace server::core::protocol {

// 고정 프레임 헤더 길이(v1.1)
inline constexpr std::size_t k_header_bytes = 14;

struct FrameHeader {
    std::uint16_t length{0};
    std::uint16_t msg_id{0};
    std::uint16_t flags{0};
    std::uint32_t seq{0};
    std::uint32_t utc_ts_ms32{0};
};

// Big-endian helpers
inline std::uint16_t read_be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}
inline std::uint32_t read_be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8)  |
           (static_cast<std::uint32_t>(p[3]));
}
inline void write_be16(std::uint16_t v, std::uint8_t* out) {
    out[0] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    out[1] = static_cast<std::uint8_t>(v & 0xFF);
}
inline void write_be32(std::uint32_t v, std::uint8_t* out) {
    out[0] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
    out[1] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    out[2] = static_cast<std::uint8_t>((v >> 8)  & 0xFF);
    out[3] = static_cast<std::uint8_t>( v       & 0xFF);
}

inline void encode_header(const FrameHeader& h, std::uint8_t* out14) {
    write_be16(h.length, out14 + 0);
    write_be16(h.msg_id, out14 + 2);
    write_be16(h.flags,  out14 + 4);
    write_be32(h.seq,    out14 + 6);
    write_be32(h.utc_ts_ms32, out14 + 10);
}
inline void decode_header(const std::uint8_t* in14, FrameHeader& h) {
    h.length = read_be16(in14 + 0);
    h.msg_id = read_be16(in14 + 2);
    h.flags  = read_be16(in14 + 4);
    h.seq    = read_be32(in14 + 6);
    h.utc_ts_ms32 = read_be32(in14 + 10);
}

// UTF-8 간단 검증(빠른 선형 스캔; 엄격 검증은 필요 시 교체)
inline bool is_valid_utf8(std::span<const std::uint8_t> s) {
    std::size_t i = 0, n = s.size();
    while (i < n) {
        auto c = s[i];
        if (c < 0x80) { i++; continue; }
        std::size_t extra = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 99;
        if (extra == 99 || i + extra >= n) return false;
        // 컨티뉴에이션 바이트 확인
        for (std::size_t k = 1; k <= extra; ++k) if ((s[i + k] & 0xC0) != 0x80) return false;
        i += extra + 1;
    }
    return true;
}

// length-prefixed UTF-8 문자열 쓰기/읽기
inline void write_lp_utf8(std::vector<std::uint8_t>& out, std::string_view str) {
    if (str.size() > 0xFFFF) str = str.substr(0, 0xFFFF);
    std::uint16_t len = static_cast<std::uint16_t>(str.size());
    std::size_t off = out.size();
    out.resize(off + 2 + len);
    write_be16(len, out.data() + off);
    if (len) std::memcpy(out.data() + off + 2, str.data(), len);
}

inline bool read_lp_utf8(std::span<const std::uint8_t>& in, std::string& out) {
    if (in.size() < 2) return false;
    std::uint16_t len = read_be16(in.data());
    if (in.size() < 2 + len) return false;
    auto sv = std::span<const std::uint8_t>(in.data() + 2, len);
    if (!is_valid_utf8(sv)) return false;
    out.assign(reinterpret_cast<const char*>(sv.data()), sv.size());
    in = in.subspan(2 + len);
    return true;
}

} // namespace server::core::protocol

