#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <span>

#include "server/core/protocol/opcode_policy.hpp"

namespace server::core {

class Session;

/**
 * @brief opcode(msg_id) 기반으로 패킷 핸들러를 라우팅하는 디스패처입니다.
 *
 * 네트워크 계층은 msg_id만 해석하고,
 * 실제 비즈니스 처리(`ChatService`)는 등록된 핸들러로 위임해 계층 분리를 유지합니다.
 */
class Dispatcher {
public:
    using handler_t = std::function<void(Session&, std::span<const std::uint8_t>)>;
    using policy_t = server::core::protocol::OpcodePolicy;

    /**
     * @brief 특정 msg_id에 대한 핸들러를 등록합니다.
     * @param msg_id 프로토콜 메시지 ID
     * @param handler 수신 payload 처리 콜백
     */
    void register_handler(
        std::uint16_t msg_id,
        handler_t handler,
        policy_t policy = server::core::protocol::default_opcode_policy());

    /**
     * @brief msg_id에 맞는 핸들러를 찾아 실행합니다.
     * @param msg_id 프로토콜 메시지 ID
     * @param s 현재 세션
     * @param payload 메시지 본문 payload
     * @return 핸들러를 찾아 실행했으면 true, 등록되지 않았으면 false
     */
    bool dispatch(std::uint16_t msg_id, Session& s, std::span<const std::uint8_t> payload) const;

    /**
     * @brief msg_id에 맞는 핸들러를 transport 문맥과 함께 실행합니다.
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
    /** @brief 등록 핸들러와 opcode 정책을 함께 보관하는 테이블 엔트리입니다. */
    struct Entry {
        handler_t handler;
        policy_t policy;
    };

    std::unordered_map<std::uint16_t, Entry> table_;
};

} // namespace server::core
