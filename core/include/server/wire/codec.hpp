// 자동 생성 파일: tools/gen_wire_codec.py에 의해 생성됨
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "wire.pb.h"

namespace server { namespace wire { namespace codec {

// MsgId<T> 템플릿 기본 정의
template<typename T>
constexpr std::uint16_t MsgId() { return 0; }

// MsgId<T> 특수화: Protobuf 타입 -> msg_id
template<> inline constexpr std::uint16_t MsgId<server::wire::v1::LoginRes>() { return 0x0011; }
template<> inline constexpr std::uint16_t MsgId<server::wire::v1::ChatBroadcast>() { return 0x0101; }
template<> inline constexpr std::uint16_t MsgId<server::wire::v1::StateSnapshot>() { return 0x0200; }
template<> inline constexpr std::uint16_t MsgId<server::wire::v1::RoomUsers>() { return 0x0201; }

// Encode helpers: Protobuf -> payload bytes
inline std::vector<std::uint8_t> Encode(const server::wire::v1::LoginRes& m) { std::string bytes; m.SerializeToString(&bytes); return std::vector<std::uint8_t>(bytes.begin(), bytes.end()); }
inline std::vector<std::uint8_t> Encode(const server::wire::v1::ChatBroadcast& m) { std::string bytes; m.SerializeToString(&bytes); return std::vector<std::uint8_t>(bytes.begin(), bytes.end()); }
inline std::vector<std::uint8_t> Encode(const server::wire::v1::StateSnapshot& m) { std::string bytes; m.SerializeToString(&bytes); return std::vector<std::uint8_t>(bytes.begin(), bytes.end()); }
inline std::vector<std::uint8_t> Encode(const server::wire::v1::RoomUsers& m) { std::string bytes; m.SerializeToString(&bytes); return std::vector<std::uint8_t>(bytes.begin(), bytes.end()); }

// Decode helpers: payload bytes -> Protobuf
inline bool Decode(const void* data, std::size_t size, server::wire::v1::LoginRes& out) { return out.ParseFromArray(data, static_cast<int>(size)); }
inline bool Decode(const void* data, std::size_t size, server::wire::v1::ChatBroadcast& out) { return out.ParseFromArray(data, static_cast<int>(size)); }
inline bool Decode(const void* data, std::size_t size, server::wire::v1::StateSnapshot& out) { return out.ParseFromArray(data, static_cast<int>(size)); }
inline bool Decode(const void* data, std::size_t size, server::wire::v1::RoomUsers& out) { return out.ParseFromArray(data, static_cast<int>(size)); }

// Utility: msg_id로 타입명을 얻기(디버그)
inline const char* TypeName(std::uint16_t id) {
  switch (id) {
    case 0x0011: return "server::wire::v1::LoginRes";
    case 0x0101: return "server::wire::v1::ChatBroadcast";
    case 0x0200: return "server::wire::v1::StateSnapshot";
    case 0x0201: return "server::wire::v1::RoomUsers";
    default: return "(unknown)";
  }
}

}}} // namespace server::wire::codec

