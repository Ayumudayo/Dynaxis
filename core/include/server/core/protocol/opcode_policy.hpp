#pragma once

#include <cstdint>

namespace server::core::protocol {

/** @brief opcode를 허용하기 전에 필요한 세션 상태입니다. */
enum class SessionStatus : std::uint8_t {
    kAny = 0,       ///< 세션 상태 제약이 없습니다.
    kAuthenticated, ///< 인증된 세션이어야 합니다.
    kInRoom,        ///< 룸에 참여한 세션이어야 합니다.
    kAdmin,         ///< 관리자 권한 세션이어야 합니다.
};

/** @brief opcode 처리 실행 위치입니다. */
enum class ProcessingPlace : std::uint8_t {
    kInline = 0, ///< 현재 실행 경로에서 즉시 처리합니다.
    kWorker,     ///< worker 큐를 통해 처리합니다.
    kRoomStrand, ///< 룸 직렬화 strand에서 처리합니다.
};

/** @brief opcode 허용 전송 계층 마스크입니다. */
enum class TransportMask : std::uint8_t {
    kNone = 0, ///< 모든 전송 계층에서 거부합니다.
    kTcp  = 1, ///< TCP만 허용합니다.
    kUdp  = 2, ///< UDP만 허용합니다.
    kBoth = 3, ///< TCP와 UDP를 모두 허용합니다.
};

/** @brief opcode 트래픽 전달 보장 등급입니다. */
enum class DeliveryClass : std::uint8_t {
    kReliableOrdered = 0, ///< 신뢰성과 순서를 모두 보장합니다.
    kReliable,            ///< 신뢰성은 보장하지만 순서는 보장하지 않습니다.
    kUnreliableSequenced, ///< 비신뢰 전송이며 시퀀스 기반 보호를 적용합니다.
};

/** @brief 디스패치 검사 시 사용하는 런타임 전송 분류입니다. */
enum class TransportKind : std::uint8_t {
    kTcp = 0,
    kUdp,
};

/** @brief 각 opcode 엔트리에 부착되는 정책 메타데이터입니다. */
struct OpcodePolicy {
    SessionStatus   required_state  = SessionStatus::kAny;              ///< 필요한 세션 상태입니다.
    ProcessingPlace processing_place = ProcessingPlace::kInline;        ///< 처리 실행 위치입니다.
    TransportMask   transport       = TransportMask::kTcp;              ///< 허용 전송 계층입니다.
    DeliveryClass   delivery        = DeliveryClass::kReliableOrdered;  ///< 전달 보장 등급입니다.
    std::uint8_t    channel         = 0;                                ///< 선택적 채널 힌트입니다.
};

/**
 * @brief 레거시 opcode 정의에 사용하는 기본 정책을 반환합니다.
 * @return 기본값으로 초기화된 `OpcodePolicy`
 */
inline constexpr OpcodePolicy default_opcode_policy() noexcept {
    return OpcodePolicy{};
}

/**
 * @brief 전송 계층 마스크가 특정 전송 종류를 허용하는지 검사합니다.
 * @param mask 전송 허용 마스크
 * @param transport 검사할 런타임 전송 종류
 * @return 마스크에서 해당 전송을 허용하면 `true`
 */
inline constexpr bool transport_allows(TransportMask mask, TransportKind transport) noexcept {
    if (mask == TransportMask::kBoth) {
        return true;
    }
    if (mask == TransportMask::kNone) {
        return false;
    }
    if (transport == TransportKind::kTcp) {
        return mask == TransportMask::kTcp;
    }
    return mask == TransportMask::kUdp;
}

} // namespace server::core::protocol
