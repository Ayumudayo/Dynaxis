#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>
#include <span>

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

    /**
     * @brief 특정 msg_id에 대한 핸들러를 등록합니다.
     * @param msg_id 프로토콜 메시지 ID
     * @param handler 수신 payload 처리 콜백
     */
    void register_handler(std::uint16_t msg_id, handler_t handler);

    /**
     * @brief msg_id에 맞는 핸들러를 찾아 실행합니다.
     * @param msg_id 프로토콜 메시지 ID
     * @param s 현재 세션
     * @param payload 메시지 본문 payload
     * @return 핸들러를 찾아 실행했으면 true, 등록되지 않았으면 false
     */
    bool dispatch(std::uint16_t msg_id, Session& s, std::span<const std::uint8_t> payload) const;

private:
    std::unordered_map<std::uint16_t, handler_t> table_;
};

} // namespace server::core
