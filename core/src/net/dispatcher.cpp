#include "server/core/net/dispatcher.hpp"

#include "server/core/net/session.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/runtime_metrics.hpp"
#include "server/core/util/log.hpp"

#include <exception>

/**
 * @brief Dispatcher 핸들러 등록/실행 구현입니다.
 *
 * 핸들러 예외를 세션 단위로 흡수해 서버 프로세스 전체 중단을 막고,
 * 디스패치 실패 원인을 메트릭/로그로 남겨 운영 추적성을 확보합니다.
 */
namespace server::core {

namespace {

bool satisfies_required_state(
    server::core::protocol::SessionStatus required,
    server::core::protocol::SessionStatus current) {
    using server::core::protocol::SessionStatus;
    switch (required) {
    case SessionStatus::kAny:
        return true;
    case SessionStatus::kAuthenticated:
        return current == SessionStatus::kAuthenticated
            || current == SessionStatus::kInRoom
            || current == SessionStatus::kAdmin;
    case SessionStatus::kInRoom:
        return current == SessionStatus::kInRoom
            || current == SessionStatus::kAdmin;
    case SessionStatus::kAdmin:
        return current == SessionStatus::kAdmin;
    }
    return false;
}

} // namespace

void Dispatcher::register_handler(std::uint16_t msg_id, handler_t handler, policy_t policy) {
    // msg_id는 protocol/opcodes.json에서 정의된 메시지 ID입니다.
    // 이 함수는 서버 시작 시점에 호출되어 메시지 ID와 처리 함수를 매핑합니다.
    table_[msg_id] = Entry{std::move(handler), policy};
}

bool Dispatcher::dispatch(std::uint16_t msg_id, Session& s, std::span<const std::uint8_t> payload) const {
    return dispatch(msg_id, s, payload, server::core::protocol::TransportKind::kTcp);
}

bool Dispatcher::dispatch(std::uint16_t msg_id,
                          Session& s,
                          std::span<const std::uint8_t> payload,
                          server::core::protocol::TransportKind transport) const {
    auto it = table_.find(msg_id);
    if (it == table_.end()) return false; // 등록되지 않은 메시지는 처리하지 않습니다.

    const auto& entry = it->second;
    if (!server::core::protocol::transport_allows(entry.policy.transport, transport)) {
        try {
            s.send_error(server::core::protocol::errc::FORBIDDEN, "forbidden");
        } catch (...) {
        }
        return true;
    }

    if (!satisfies_required_state(entry.policy.required_state, s.session_status())) {
        try {
            s.send_error(server::core::protocol::errc::FORBIDDEN, "forbidden");
        } catch (...) {
        }
        return true;
    }

    switch (entry.policy.processing_place) {
    case server::core::protocol::ProcessingPlace::kInline:
    case server::core::protocol::ProcessingPlace::kWorker:
    case server::core::protocol::ProcessingPlace::kRoomStrand:
        break;
    }

    try {
        // 핸들러 실행 중 발생하는 예외는 세션 단위 오류로 처리합니다.
        // 즉, 특정 클라이언트의 요청 처리 중 오류가 발생해도 서버 전체가 죽지 않도록 방어합니다.
        entry.handler(s, payload);
    } catch (const std::exception& ex) {
        runtime_metrics::record_dispatch_exception();
        server::core::log::error(std::string("handler exception for msg=") + std::to_string(msg_id) + ": " + ex.what());
        // 클라이언트에게 내부 오류임을 알립니다.
        try {
            s.send_error(server::core::protocol::errc::INTERNAL_ERROR, "internal error");
        } catch (...) {
        }
    } catch (...) {
        runtime_metrics::record_dispatch_exception();
        server::core::log::error(std::string("handler unknown exception for msg=") + std::to_string(msg_id));
        try {
            s.send_error(server::core::protocol::errc::INTERNAL_ERROR, "internal error");
        } catch (...) {
        }
    }
    return true;
}

} // namespace server::core
