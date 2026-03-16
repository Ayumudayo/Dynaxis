// 자동 생성 파일: tools/gen_wire_codec.py에 의해 생성됨
/**
 * @file
 * @brief Protobuf wire codec 자동 생성 헤더입니다.
 * @note tools/gen_wire_codec.py로 생성됩니다. 직접 수정하지 마세요.
 */
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
template<> inline constexpr std::uint16_t MsgId<server::wire::v1::FpsInput>() { return 0x0206; }
template<> inline constexpr std::uint16_t MsgId<server::wire::v1::FpsStateSnapshot>() { return 0x0207; }
template<> inline constexpr std::uint16_t MsgId<server::wire::v1::FpsStateDelta>() { return 0x0208; }

// 인코드 헬퍼: Protobuf -> payload 바이트
inline std::vector<std::uint8_t> Encode(const server::wire::v1::LoginRes& m) { std::string bytes; m.SerializeToString(&bytes); return std::vector<std::uint8_t>(bytes.begin(), bytes.end()); }
inline std::vector<std::uint8_t> Encode(const server::wire::v1::ChatBroadcast& m) { std::string bytes; m.SerializeToString(&bytes); return std::vector<std::uint8_t>(bytes.begin(), bytes.end()); }
inline std::vector<std::uint8_t> Encode(const server::wire::v1::StateSnapshot& m) { std::string bytes; m.SerializeToString(&bytes); return std::vector<std::uint8_t>(bytes.begin(), bytes.end()); }
inline std::vector<std::uint8_t> Encode(const server::wire::v1::RoomUsers& m) { std::string bytes; m.SerializeToString(&bytes); return std::vector<std::uint8_t>(bytes.begin(), bytes.end()); }
inline std::vector<std::uint8_t> Encode(const server::wire::v1::FpsInput& m) { std::string bytes; m.SerializeToString(&bytes); return std::vector<std::uint8_t>(bytes.begin(), bytes.end()); }
inline std::vector<std::uint8_t> Encode(const server::wire::v1::FpsStateSnapshot& m) { std::string bytes; m.SerializeToString(&bytes); return std::vector<std::uint8_t>(bytes.begin(), bytes.end()); }
inline std::vector<std::uint8_t> Encode(const server::wire::v1::FpsStateDelta& m) { std::string bytes; m.SerializeToString(&bytes); return std::vector<std::uint8_t>(bytes.begin(), bytes.end()); }

// 디코드 헬퍼: payload 바이트 -> Protobuf
inline bool Decode(const void* data, std::size_t size, server::wire::v1::LoginRes& out) { return out.ParseFromArray(data, static_cast<int>(size)); }
inline bool Decode(const void* data, std::size_t size, server::wire::v1::ChatBroadcast& out) { return out.ParseFromArray(data, static_cast<int>(size)); }
inline bool Decode(const void* data, std::size_t size, server::wire::v1::StateSnapshot& out) { return out.ParseFromArray(data, static_cast<int>(size)); }
inline bool Decode(const void* data, std::size_t size, server::wire::v1::RoomUsers& out) { return out.ParseFromArray(data, static_cast<int>(size)); }
inline bool Decode(const void* data, std::size_t size, server::wire::v1::FpsInput& out) { return out.ParseFromArray(data, static_cast<int>(size)); }
inline bool Decode(const void* data, std::size_t size, server::wire::v1::FpsStateSnapshot& out) { return out.ParseFromArray(data, static_cast<int>(size)); }
inline bool Decode(const void* data, std::size_t size, server::wire::v1::FpsStateDelta& out) { return out.ParseFromArray(data, static_cast<int>(size)); }

// 유틸리티: msg_id로 타입명을 얻기(디버그)
inline const char* TypeName(std::uint16_t id) {
  switch (id) {
    case 0x0011: return "server::wire::v1::LoginRes";
    case 0x0101: return "server::wire::v1::ChatBroadcast";
    case 0x0200: return "server::wire::v1::StateSnapshot";
    case 0x0201: return "server::wire::v1::RoomUsers";
    case 0x0206: return "server::wire::v1::FpsInput";
    case 0x0207: return "server::wire::v1::FpsStateSnapshot";
    case 0x0208: return "server::wire::v1::FpsStateDelta";
    default: return "(unknown)";
  }
}

}}} // namespace server::wire::codec

