#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>

#include "server/core/protocol/opcode_policy.hpp"

namespace server::core::net {

/**
 * @brief higher-level transport session seam used by the stable public router.
 *
 * `Connection`/`Listener`가 바이트 스트림과 accept loop를 담당한다면,
 * 이 인터페이스는 "메시지를 어느 실행 문맥에서 처리할 수 있는가"를 public consumer가
 * 직접 정의할 수 있게 해 줍니다. 현재 서버의 packet `Session` 구현을 그대로 공개하지 않고도,
 * 상태 검사, error reply, serialized callback 실행이라는 최소 routing contract를 유지하려는 목적입니다.
 */
class ITransportSession {
public:
    virtual ~ITransportSession() = default;

    /**
     * @brief 현재 transport session의 인증/권한 상태를 반환합니다.
     * @return opcode policy evaluation에 사용할 현재 세션 상태
     */
    virtual server::core::protocol::SessionStatus session_status() const noexcept = 0;

    /**
     * @brief session-owned serialized execution context에 콜백을 게시합니다.
     * @param fn 순차 실행으로 예약할 콜백
     * @return 예약이 받아들여졌으면 `true`, 이미 종료되어 거부됐으면 `false`
     */
    virtual bool post_serialized(std::function<void()> fn) = 0;

    /**
     * @brief protocol-level error를 peer에게 전달합니다.
     * @param code protocol error code
     * @param message lightweight operator-facing error text
     */
    virtual void send_error(std::uint16_t code, std::string_view message) = 0;
};

/**
 * @brief domain-neutral opcode router that depends only on `ITransportSession`.
 *
 * 이 타입은 기존 `Dispatcher`가 packet `Session`에 직접 결합돼 있던 public consumer path를
 * 대체하기 위한 stable seam입니다. transport policy, required state, processing place를
 * 동일하게 적용하되, 실제 session 구현은 앱/consumer가 소유하게 둡니다.
 */
class TransportRouter {
public:
    using handler_t = std::function<void(ITransportSession&, std::span<const std::uint8_t>)>;
    using policy_t = server::core::protocol::OpcodePolicy;

    /**
     * @brief msg_id에 대한 handler와 opcode policy를 함께 등록합니다.
     * @param msg_id protocol message ID
     * @param handler payload consumer callback
     * @param policy required state / transport / processing place policy
     */
    void register_handler(
        std::uint16_t msg_id,
        handler_t handler,
        policy_t policy = server::core::protocol::default_opcode_policy());

    /**
     * @brief TCP 기본 문맥에서 msg_id에 대응하는 handler를 실행합니다.
     * @param msg_id protocol message ID
     * @param session consumer-owned transport session
     * @param payload message payload bytes
     * @return handler를 찾거나 policy rejection을 처리했으면 `true`, 미등록이면 `false`
     */
    bool dispatch(std::uint16_t msg_id,
                  const std::shared_ptr<ITransportSession>& session,
                  std::span<const std::uint8_t> payload) const;

    /**
     * @brief 실제 transport kind까지 반영해 msg_id에 대응하는 handler를 실행합니다.
     * @param msg_id protocol message ID
     * @param session consumer-owned transport session
     * @param payload message payload bytes
     * @param transport ingress transport kind
     * @return handler를 찾거나 policy rejection을 처리했으면 `true`, 미등록이면 `false`
     */
    bool dispatch(std::uint16_t msg_id,
                  const std::shared_ptr<ITransportSession>& session,
                  std::span<const std::uint8_t> payload,
                  server::core::protocol::TransportKind transport) const;

private:
    /** @brief msg_id별 handler/policy 한 쌍을 저장하는 내부 테이블 엔트리입니다. */
    struct Entry {
        handler_t handler;
        policy_t policy;
    };

    std::unordered_map<std::uint16_t, Entry> table_;
};

} // namespace server::core::net
