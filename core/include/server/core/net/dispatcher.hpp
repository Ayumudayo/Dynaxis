#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <span>

#include "server/core/protocol/opcode_policy.hpp"

namespace server::core {

class Session;

/**
 * @brief opcode 기반으로 비즈니스 핸들러를 찾는 얇은 라우팅 테이블입니다.
 *
 * 네트워크 계층은 프레이밍과 세션 수명만 다루고,
 * 실제 의미 해석은 등록된 핸들러로 넘겨 계층 경계를 유지합니다.
 * 이렇게 해야 transport 계층이 채팅/게임 규칙을 직접 알지 않게 되고,
 * 새 opcode를 추가할 때도 accept/read/write 코드까지 건드리는 일을 피할 수 있습니다.
 *
 * 또한 `OpcodePolicy`를 엔트리와 함께 보관해 "어떤 메시지가 어느 transport에서 허용되는가"를
 * 라우팅 순간에 함께 판단합니다. 정책과 핸들러를 따로 두면,
 * 등록은 되어 있는데 transport 제한은 잊어버리는 식의 운영 사고가 나기 쉽습니다.
 */
class Dispatcher {
public:
    using handler_t = std::function<void(Session&, std::span<const std::uint8_t>)>;
    using policy_t = server::core::protocol::OpcodePolicy;

    /**
     * @brief 특정 msg_id에 대한 핸들러와 transport 정책을 함께 등록합니다.
     *
     * 등록 시 정책을 같이 묶는 이유는, 나중에 조회하는 쪽이 별도 정책 테이블과
     * 동기화될 것이라고 기대하지 않게 만들기 위해서입니다.
     * 핸들러와 정책이 분리되면 일부 opcode는 구현돼 있는데도 잘못된 경로에서 열릴 수 있습니다.
     *
     * @param msg_id 프로토콜 메시지 ID
     * @param handler 수신 payload 처리 콜백
     */
    void register_handler(
        std::uint16_t msg_id,
        handler_t handler,
        policy_t policy = server::core::protocol::default_opcode_policy());

    /**
     * @brief 기본 transport 문맥에서 msg_id에 대응하는 핸들러를 실행합니다.
     *
     * transport를 별도로 전달하지 않는 호출은 기존 TCP 중심 경로와의 호환을 위한 얇은 편의층입니다.
     * 신규 경로는 가능한 한 아래 transport-aware 오버로드를 쓰는 편이 정책 누락을 줄입니다.
     *
     * @param msg_id 프로토콜 메시지 ID
     * @param s 현재 세션
     * @param payload 메시지 본문 payload
     * @return 핸들러를 찾아 실행했으면 true, 등록되지 않았으면 false
     */
    bool dispatch(std::uint16_t msg_id, Session& s, std::span<const std::uint8_t> payload) const;

    /**
     * @brief 실제 수신 transport 문맥까지 반영해 핸들러를 실행합니다.
     *
     * 같은 opcode라도 TCP에서는 허용되지만 UDP에서는 금지될 수 있습니다.
     * 이 판단을 핸들러 내부로 미루면 각 서비스가 중복 검사를 구현하게 되고,
     * 누락된 경로가 생기면 잘못된 transport에서 메시지가 통과할 수 있습니다.
     *
     * @param msg_id 프로토콜 메시지 ID
     * @param s 현재 세션
     * @param payload 메시지 본문 payload
     * @param transport 실제 수신 전송 계층(TCP/UDP)
     * @return 핸들러를 찾아 실행했으면 true, 등록되지 않았으면 false
     */
    bool dispatch(std::uint16_t msg_id,
                  Session& s,
                  std::span<const std::uint8_t> payload,
                  server::core::protocol::TransportKind transport) const;

private:
    /** @brief 등록 핸들러와 opcode 정책을 원자적으로 함께 보관하는 테이블 엔트리입니다. */
    struct Entry {
        handler_t handler;
        policy_t policy;
    };

    std::unordered_map<std::uint16_t, Entry> table_;
};

} // namespace server::core
