// 자동 생성 파일: tools/gen_wire_codec.py에 의해 생성됨
/**
 * @file
 * @brief Protobuf 메시지 타입과 wire msg_id 매핑, 인코드/디코드 헬퍼를 한곳에서 자동 생성한 헤더입니다.
 *
 * 직렬화 헬퍼를 손으로 유지하면 새 메시지 추가 때 `MsgId`, `Encode`, `Decode`, `TypeName` 중 하나를 빼먹기 쉽습니다.
 * 이 헤더는 `wire_map.json`을 기준으로 필요한 얇은 glue code를 한 번에 생성해, 메시지 목록과 codec 표가 같이 움직이게 합니다.
 *
 * @note tools/gen_wire_codec.py와 `protocol/wire_map.json`에서 다시 생성됩니다. 직접 수정하면 다음 생성 때 덮어써집니다.
 */
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "wire.pb.h"

namespace server { namespace wire { namespace codec {

// MsgId<T> 기본 템플릿
// 매핑이 없는 타입은 0을 돌려 "wire에 올리면 안 되는 타입"임을 눈에 띄게 남깁니다.
template<typename T>
constexpr std::uint16_t MsgId() { return 0; }

// MsgId<T> 특수화: Protobuf 타입 -> msg_id
// 타입별 상수를 자동 생성해 호출부가 별도 switch를 중복 구현하지 않게 합니다.
template<> inline constexpr std::uint16_t MsgId<server::wire::v1::LoginRes>() { return 0x0011; }
template<> inline constexpr std::uint16_t MsgId<server::wire::v1::ChatBroadcast>() { return 0x0101; }
template<> inline constexpr std::uint16_t MsgId<server::wire::v1::StateSnapshot>() { return 0x0200; }
template<> inline constexpr std::uint16_t MsgId<server::wire::v1::RoomUsers>() { return 0x0201; }
template<> inline constexpr std::uint16_t MsgId<server::wire::v1::FpsInput>() { return 0x0206; }
template<> inline constexpr std::uint16_t MsgId<server::wire::v1::FpsStateSnapshot>() { return 0x0207; }
template<> inline constexpr std::uint16_t MsgId<server::wire::v1::FpsStateDelta>() { return 0x0208; }

// 인코드 헬퍼: Protobuf -> payload 바이트
// 호출부는 "어떤 타입을 어떤 바이트로 보낼까"만 알고 있으면 되고, 직렬화 boilerplate는 여기서 숨깁니다.
inline std::vector<std::uint8_t> Encode(const server::wire::v1::LoginRes& m) { std::string bytes; m.SerializeToString(&bytes); return std::vector<std::uint8_t>(bytes.begin(), bytes.end()); }
inline std::vector<std::uint8_t> Encode(const server::wire::v1::ChatBroadcast& m) { std::string bytes; m.SerializeToString(&bytes); return std::vector<std::uint8_t>(bytes.begin(), bytes.end()); }
inline std::vector<std::uint8_t> Encode(const server::wire::v1::StateSnapshot& m) { std::string bytes; m.SerializeToString(&bytes); return std::vector<std::uint8_t>(bytes.begin(), bytes.end()); }
inline std::vector<std::uint8_t> Encode(const server::wire::v1::RoomUsers& m) { std::string bytes; m.SerializeToString(&bytes); return std::vector<std::uint8_t>(bytes.begin(), bytes.end()); }
inline std::vector<std::uint8_t> Encode(const server::wire::v1::FpsInput& m) { std::string bytes; m.SerializeToString(&bytes); return std::vector<std::uint8_t>(bytes.begin(), bytes.end()); }
inline std::vector<std::uint8_t> Encode(const server::wire::v1::FpsStateSnapshot& m) { std::string bytes; m.SerializeToString(&bytes); return std::vector<std::uint8_t>(bytes.begin(), bytes.end()); }
inline std::vector<std::uint8_t> Encode(const server::wire::v1::FpsStateDelta& m) { std::string bytes; m.SerializeToString(&bytes); return std::vector<std::uint8_t>(bytes.begin(), bytes.end()); }

// 디코드 헬퍼: payload 바이트 -> Protobuf
// 디코드 규칙을 한곳에 두면 ParseFromArray 호출 방식이 메시지마다 미묘하게 갈라지지 않습니다.
inline bool Decode(const void* data, std::size_t size, server::wire::v1::LoginRes& out) { return out.ParseFromArray(data, static_cast<int>(size)); }
inline bool Decode(const void* data, std::size_t size, server::wire::v1::ChatBroadcast& out) { return out.ParseFromArray(data, static_cast<int>(size)); }
inline bool Decode(const void* data, std::size_t size, server::wire::v1::StateSnapshot& out) { return out.ParseFromArray(data, static_cast<int>(size)); }
inline bool Decode(const void* data, std::size_t size, server::wire::v1::RoomUsers& out) { return out.ParseFromArray(data, static_cast<int>(size)); }
inline bool Decode(const void* data, std::size_t size, server::wire::v1::FpsInput& out) { return out.ParseFromArray(data, static_cast<int>(size)); }
inline bool Decode(const void* data, std::size_t size, server::wire::v1::FpsStateSnapshot& out) { return out.ParseFromArray(data, static_cast<int>(size)); }
inline bool Decode(const void* data, std::size_t size, server::wire::v1::FpsStateDelta& out) { return out.ParseFromArray(data, static_cast<int>(size)); }

// 유틸리티: msg_id로 타입명을 얻기
// 로그/디버그/진단에서 숫자 ID만 보는 것보다 타입명을 함께 보는 편이 훨씬 빠르게 맥락을 파악할 수 있습니다.
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

