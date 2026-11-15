#pragma once

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

/** @brief 표시 문자열과 영속 메시지 식별자를 함께 보관하는 로그 항목입니다. */
struct ChatLogLine {
    std::string text;
    std::string room_id;
    std::uint64_t message_id{};
};

/**
 * @brief snapshot과 live 메시지를 같은 ID로 병합하는 UI 독립 모델입니다.
 *
 * watermark보다 작은 지연 메시지도 받아들이며 같은 ID만 제거합니다.
 * 방 writer의 commit 순서와 네트워크 도착 순서는 다를 수 있으므로 max_id만으로 버리지 않습니다.
 */
class MessageTimeline {
public:
    std::vector<ChatLogLine> lines;
    /** @brief 일시적인 시스템/귓속말 알림을 추가합니다. */
    void add_notice(std::string text) { lines.push_back({std::move(text), {}, 0}); trim(); }
    /** @brief 영속 메시지를 중복 없이 ID 순서에 맞게 삽입합니다. */
    void add_message(const std::string& room_id, std::uint64_t id, std::string text) {
        if (id == 0 || room_id.empty()) { add_notice(std::move(text)); return; }
        if (std::any_of(lines.begin(), lines.end(), [&](const auto& line) { return line.room_id == room_id && line.message_id == id; })) return;
        const auto position = std::find_if(lines.begin(), lines.end(), [&](const auto& line) { return line.room_id == room_id && line.message_id > id; });
        lines.insert(position, {std::move(text), room_id, id});
        trim();
    }
    /** @brief 방 세대와 확정 snapshot 경계를 기록합니다. 과거 메시지 ID를 누락 판정에 쓰지 않습니다. */
    void snapshot(const std::string& room, const std::string& room_id, std::uint64_t watermark) {
        if (room_id.empty()) return;
        auto& previous = rooms_[room];
        if (!previous.empty() && previous != room_id) std::erase_if(lines, [&](const auto& line) { return line.room_id == previous; });
        previous = room_id;
        watermarks_[room_id] = std::max(watermarks_[room_id], watermark);
    }
private:
    void trim() { if (lines.size() > 2000) lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(lines.size() - 2000)); }
    std::unordered_map<std::string, std::string> rooms_;
    std::unordered_map<std::string, std::uint64_t> watermarks_;
};
