#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gateway {

/**
 * @brief gateway 세션이 backend로 payload를 전달하기 위한 전송 인터페이스입니다.
 *
 * TCP/UDP 유입 경로는 이 인터페이스만 사용해 backend 전달을 수행하고,
 * 실제 전송 구현(TCP bridge, 향후 UDP/RUDP direct path)은 구현체로 분리합니다.
 */
class ITransportSession {
public:
    virtual ~ITransportSession() = default;
    virtual void send(std::vector<std::uint8_t> payload) = 0;
    virtual void send(const std::uint8_t* data, std::size_t length) = 0;
    virtual void close() = 0;
    virtual const std::string& session_id() const = 0;
};

using TransportSessionPtr = std::shared_ptr<ITransportSession>;

} // namespace gateway
