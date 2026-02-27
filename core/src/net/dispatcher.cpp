#include "server/core/net/dispatcher.hpp"

#include "server/core/concurrent/job_queue.hpp"
#include "server/core/net/session.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/runtime_metrics.hpp"
#include "server/core/util/log.hpp"
#include "server/core/util/service_registry.hpp"

#include <exception>
#include <memory>
#include <vector>

/**
 * @brief Dispatcher 핸들러 등록/실행 구현입니다.
 *
 * 핸들러 예외를 세션 단위로 흡수해 서버 프로세스 전체 중단을 막고,
 * 디스패치 실패 원인을 메트릭/로그로 남겨 운영 추적성을 확보합니다.
 */
namespace server::core {

namespace {

namespace services = server::core::util::services;

constexpr std::size_t kDispatchPlaceInlineIndex = 0;
constexpr std::size_t kDispatchPlaceWorkerIndex = 1;
constexpr std::size_t kDispatchPlaceRoomStrandIndex = 2;

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

std::size_t processing_place_index(server::core::protocol::ProcessingPlace place) {
    using server::core::protocol::ProcessingPlace;
    switch (place) {
    case ProcessingPlace::kInline:
        return kDispatchPlaceInlineIndex;
    case ProcessingPlace::kWorker:
        return kDispatchPlaceWorkerIndex;
    case ProcessingPlace::kRoomStrand:
        return kDispatchPlaceRoomStrandIndex;
    }
    return server::core::runtime_metrics::kDispatchProcessingPlaceCount;
}

const char* processing_place_name(server::core::protocol::ProcessingPlace place) {
    using server::core::protocol::ProcessingPlace;
    switch (place) {
    case ProcessingPlace::kInline:
        return "inline";
    case ProcessingPlace::kWorker:
        return "worker";
    case ProcessingPlace::kRoomStrand:
        return "room_strand";
    }
    return "unknown";
}

void send_error_noexcept(Session& session, std::uint16_t code, const char* message) {
    try {
        session.send_error(code, message);
    } catch (...) {
    }
}

std::shared_ptr<Session> shared_session_or_null(Session& session) {
    try {
        return session.shared_from_this();
    } catch (...) {
        return nullptr;
    }
}

void run_handler_with_guard(const Dispatcher::handler_t& handler,
                            Session& session,
                            std::span<const std::uint8_t> payload,
                            std::uint16_t msg_id,
                            std::size_t place_index) {
    try {
        handler(session, payload);
    } catch (const std::exception& ex) {
        server::core::runtime_metrics::record_dispatch_exception();
        server::core::runtime_metrics::record_dispatch_processing_place_exception(place_index);
        server::core::log::error(
            std::string("handler exception for msg=") + std::to_string(msg_id) +
            " place=" + std::to_string(place_index) + ": " + ex.what());
        send_error_noexcept(session, server::core::protocol::errc::INTERNAL_ERROR, "internal error");
    } catch (...) {
        server::core::runtime_metrics::record_dispatch_exception();
        server::core::runtime_metrics::record_dispatch_processing_place_exception(place_index);
        server::core::log::error(
            std::string("handler unknown exception for msg=") + std::to_string(msg_id) +
            " place=" + std::to_string(place_index));
        send_error_noexcept(session, server::core::protocol::errc::INTERNAL_ERROR, "internal error");
    }
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

    const auto place = entry.policy.processing_place;
    const auto place_index = processing_place_index(place);
    if (place_index >= runtime_metrics::kDispatchProcessingPlaceCount) {
        server::core::log::error(
            std::string("unsupported processing_place for msg=") + std::to_string(msg_id));
        send_error_noexcept(s, server::core::protocol::errc::INTERNAL_ERROR, "unsupported processing place");
        return true;
    }

    runtime_metrics::record_dispatch_processing_place_call(place_index);

    if (place == server::core::protocol::ProcessingPlace::kInline) {
        run_handler_with_guard(entry.handler, s, payload, msg_id, place_index);
        return true;
    }

    auto session = shared_session_or_null(s);
    if (!session) {
        runtime_metrics::record_dispatch_processing_place_reject(place_index);
        server::core::log::warn(
            std::string("processing_place rejected: missing shared session for msg=") +
            std::to_string(msg_id) + " place=" + processing_place_name(place));
        send_error_noexcept(s, server::core::protocol::errc::SERVER_BUSY, "dispatch context unavailable");
        return true;
    }

    auto handler = entry.handler;
    std::vector<std::uint8_t> payload_copy(payload.begin(), payload.end());

    auto invoke_on_session = [handler = std::move(handler),
                              session,
                              payload_copy = std::move(payload_copy),
                              msg_id,
                              place_index]() mutable {
        run_handler_with_guard(
            handler,
            *session,
            std::span<const std::uint8_t>(payload_copy.data(), payload_copy.size()),
            msg_id,
            place_index);
    };

    if (place == server::core::protocol::ProcessingPlace::kRoomStrand) {
        const bool accepted = session->post_serialized(std::move(invoke_on_session));
        if (!accepted) {
            runtime_metrics::record_dispatch_processing_place_reject(place_index);
            send_error_noexcept(s, server::core::protocol::errc::SERVER_BUSY, "dispatch context unavailable");
        }
        return true;
    }

    auto job_queue = services::get<server::core::JobQueue>();
    if (!job_queue) {
        runtime_metrics::record_dispatch_processing_place_reject(place_index);
        server::core::log::warn(
            std::string("processing_place rejected: worker queue unavailable for msg=") +
            std::to_string(msg_id));
        send_error_noexcept(s, server::core::protocol::errc::SERVER_BUSY, "worker queue unavailable");
        return true;
    }

    const bool queued = job_queue->TryPush([session, task = std::move(invoke_on_session), place_index]() mutable {
        const bool accepted = session->post_serialized(std::move(task));
        if (!accepted) {
            runtime_metrics::record_dispatch_processing_place_reject(place_index);
            send_error_noexcept(*session, server::core::protocol::errc::SERVER_BUSY, "dispatch context unavailable");
        }
    });

    if (!queued) {
        runtime_metrics::record_dispatch_processing_place_reject(place_index);
        send_error_noexcept(s, server::core::protocol::errc::SERVER_BUSY, "worker queue full");
    }
    return true;
}

} // namespace server::core
