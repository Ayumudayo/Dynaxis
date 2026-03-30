#pragma once

#include "server/core/net/session.hpp"
#include "wire.pb.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace server::app::chat {

/**
 * @brief 로컬/분산 fanout에 공통으로 쓰는 직렬화된 채팅 브로드캐스트 묶음입니다.
 *
 * wire payload, async_send body, recent-history snapshot payload가 같은 시각 기준으로
 * 함께 움직여야 fanout/refresh/캐시 정합성이 유지됩니다.
 */
struct PreparedChatBroadcast {
    std::uint64_t ts_ms{};
    std::string serialized_payload{};
    std::vector<std::uint8_t> body{};
    server::wire::v1::StateSnapshot::SnapshotMessage snapshot_message{};
};

PreparedChatBroadcast prepare_chat_broadcast(std::string_view room,
                                             std::string_view sender,
                                             std::string_view text,
                                             std::uint32_t sender_session_id);

void send_local_chat_broadcast(
    const std::shared_ptr<server::core::net::Session>& sender_session,
    const std::vector<std::shared_ptr<server::core::net::Session>>& targets,
    const std::vector<std::uint8_t>& body);

} // namespace server::app::chat
