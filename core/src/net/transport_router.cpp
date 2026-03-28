#include "server/core/net/transport_router.hpp"

#include "server/core/concurrent/job_queue.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/runtime_metrics.hpp"
#include "server/core/util/log.hpp"
#include "server/core/util/service_registry.hpp"

#include <exception>
#include <vector>

namespace server::core::net {

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

void send_error_noexcept(ITransportSession& session, std::uint16_t code, const char* message) {
    try {
        session.send_error(code, message);
    } catch (...) {
        server::core::runtime_metrics::record_exception_ignored();
    }
}

void run_handler_with_guard(const TransportRouter::handler_t& handler,
                            ITransportSession& session,
                            std::span<const std::uint8_t> payload,
                            std::uint16_t msg_id,
                            std::size_t place_index) {
    try {
        handler(session, payload);
    } catch (const std::exception& ex) {
        server::core::runtime_metrics::record_dispatch_exception();
        server::core::runtime_metrics::record_dispatch_processing_place_exception(place_index);
        server::core::runtime_metrics::record_exception_recoverable();
        server::core::log::error(
            std::string("component=transport_router error_code=INTERNAL_ERROR handler exception for msg=") +
            std::to_string(msg_id) + " place=" + std::to_string(place_index) + ": " + ex.what());
        send_error_noexcept(session, server::core::protocol::errc::INTERNAL_ERROR, "internal error");
    } catch (...) {
        server::core::runtime_metrics::record_dispatch_exception();
        server::core::runtime_metrics::record_dispatch_processing_place_exception(place_index);
        server::core::runtime_metrics::record_exception_ignored();
        server::core::log::error(
            std::string("component=transport_router error_code=INTERNAL_ERROR handler unknown exception for msg=") +
            std::to_string(msg_id) + " place=" + std::to_string(place_index));
        send_error_noexcept(session, server::core::protocol::errc::INTERNAL_ERROR, "internal error");
    }
}

} // namespace

void TransportRouter::register_handler(std::uint16_t msg_id, handler_t handler, policy_t policy) {
    table_[msg_id] = Entry{std::move(handler), policy};
}

bool TransportRouter::dispatch(std::uint16_t msg_id,
                               const std::shared_ptr<ITransportSession>& session,
                               std::span<const std::uint8_t> payload) const {
    return dispatch(msg_id, session, payload, server::core::protocol::TransportKind::kTcp);
}

bool TransportRouter::dispatch(std::uint16_t msg_id,
                               const std::shared_ptr<ITransportSession>& session,
                               std::span<const std::uint8_t> payload,
                               server::core::protocol::TransportKind transport) const {
    if (!session) {
        return false;
    }

    const auto it = table_.find(msg_id);
    if (it == table_.end()) {
        return false;
    }

    const auto& entry = it->second;
    if (!server::core::protocol::transport_allows(entry.policy.transport, transport)) {
        send_error_noexcept(*session, server::core::protocol::errc::FORBIDDEN, "forbidden");
        return true;
    }

    if (!satisfies_required_state(entry.policy.required_state, session->session_status())) {
        send_error_noexcept(*session, server::core::protocol::errc::FORBIDDEN, "forbidden");
        return true;
    }

    const auto place = entry.policy.processing_place;
    const auto place_index = processing_place_index(place);
    if (place_index >= runtime_metrics::kDispatchProcessingPlaceCount) {
        server::core::log::error(
            std::string("component=transport_router error_code=INTERNAL_ERROR unsupported processing_place for msg=") +
            std::to_string(msg_id));
        send_error_noexcept(*session,
                            server::core::protocol::errc::INTERNAL_ERROR,
                            "unsupported processing place");
        return true;
    }

    runtime_metrics::record_dispatch_processing_place_call(place_index);

    if (place == server::core::protocol::ProcessingPlace::kInline) {
        run_handler_with_guard(entry.handler, *session, payload, msg_id, place_index);
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
            send_error_noexcept(*session,
                                server::core::protocol::errc::SERVER_BUSY,
                                "dispatch context unavailable");
        }
        return true;
    }

    auto job_queue = services::get<server::core::JobQueue>();
    if (!job_queue) {
        runtime_metrics::record_dispatch_processing_place_reject(place_index);
        server::core::log::warn(
            std::string("component=transport_router error_code=SERVER_BUSY processing_place rejected: worker queue unavailable for msg=") +
            std::to_string(msg_id));
        send_error_noexcept(*session,
                            server::core::protocol::errc::SERVER_BUSY,
                            "worker queue unavailable");
        return true;
    }

    const bool queued = job_queue->TryPush([session, task = std::move(invoke_on_session), place_index]() mutable {
        const bool accepted = session->post_serialized(std::move(task));
        if (!accepted) {
            runtime_metrics::record_dispatch_processing_place_reject(place_index);
            send_error_noexcept(*session,
                                server::core::protocol::errc::SERVER_BUSY,
                                "dispatch context unavailable");
        }
    });

    if (!queued) {
        runtime_metrics::record_dispatch_processing_place_reject(place_index);
        send_error_noexcept(*session,
                            server::core::protocol::errc::SERVER_BUSY,
                            "worker queue full");
    }
    return true;
}

} // namespace server::core::net
