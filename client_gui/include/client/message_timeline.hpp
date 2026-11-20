#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

/**
 * @brief 타임라인 항목의 메시지 성격을 구분하는 열거형입니다.
 *
 * UI 계층이 문자열 접두 파싱에 의존하지 않고 채팅·귓속말·시스템·오류 항목을
 * 각각 다른 배지와 색상, 필터 탭으로 렌더링할 수 있도록 분리합니다.
 */
enum class ChatLineKind {
    CHAT,
    WHISPER_IN,
    WHISPER_OUT,
    SYSTEM_NOTICE,
    ERROR_NOTICE
};

/**
 * @brief 구조화된 표시 필드와 영속 메시지 식별자를 함께 보관하는 타임라인 항목입니다.
 */
struct ChatLogLine {
    ChatLineKind kind{ChatLineKind::SYSTEM_NOTICE};
    std::string text;
    std::string room_id;
    std::string room_name;
    std::uint64_t message_id{};
    std::string sender;
    std::string recipient;
    std::uint64_t timestamp_ms{};
};

/**
 * @brief snapshot과 live 메시지를 같은 ID로 병합하는 UI 독립 타임라인 모델입니다.
 *
 * watermark보다 작은 지연 메시지도 받아들이며 같은 `(room_id, message_id)`만 제거합니다.
 * 방 writer의 commit 순서와 네트워크 도착 순서는 다를 수 있으므로 max_id만으로 버리지 않습니다.
 */
class MessageTimeline {
public:
    std::vector<ChatLogLine> lines;

    /**
     * @brief 일시적인 시스템 안내 알림을 추가합니다.
     * @param text 표시할 시스템 안내 문자열
     * @param timestamp_ms 밀리초 단위 시각(선택)
     */
    void add_notice(std::string text, std::uint64_t timestamp_ms = 0) {
        ChatLogLine line;
        line.kind = ChatLineKind::SYSTEM_NOTICE;
        line.text = std::move(text);
        line.timestamp_ms = timestamp_ms;
        lines.push_back(std::move(line));
        trim();
    }

    /**
     * @brief 오류 또는 거절 알림을 추가합니다.
     * @param text 표시할 오류 메시지
     * @param timestamp_ms 밀리초 단위 시각(선택)
     */
    void add_error(std::string text, std::uint64_t timestamp_ms = 0) {
        ChatLogLine line;
        line.kind = ChatLineKind::ERROR_NOTICE;
        line.text = std::move(text);
        line.timestamp_ms = timestamp_ms;
        lines.push_back(std::move(line));
        trim();
    }

    /**
     * @brief 수신 또는 발신 귓속말 항목을 추가합니다.
     * @param sender 발신자 식별자
     * @param recipient 수신자 식별자
     * @param text 귓속말 본문
     * @param outgoing 사용자가 보낸 귓속말이면 true
     * @param timestamp_ms 밀리초 단위 시각(선택)
     */
    void add_whisper(std::string sender,
                     std::string recipient,
                     std::string text,
                     bool outgoing,
                     std::uint64_t timestamp_ms = 0) {
        ChatLogLine line;
        line.kind = outgoing ? ChatLineKind::WHISPER_OUT : ChatLineKind::WHISPER_IN;
        line.sender = std::move(sender);
        line.recipient = std::move(recipient);
        line.text = std::move(text);
        line.timestamp_ms = timestamp_ms;
        lines.push_back(std::move(line));
        trim();
    }

    /**
     * @brief 구조화된 방 채팅 메시지를 `(room_id, message_id)` 기준으로 중복 제거 및 정렬 삽입합니다.
     * @param room_id 영속 방 세대 UUID
     * @param room_name 표시용 방 이름
     * @param id 방 내 영속 commit 메시지 ID (0이면 시스템 알림으로 취급)
     * @param sender 발신자 이름
     * @param text 채팅 본문
     * @param timestamp_ms 서버 또는 수신 기준 밀리초 타임스탬프
     */
    void add_chat_message(const std::string& room_id,
                          const std::string& room_name,
                          std::uint64_t id,
                          std::string sender,
                          std::string text,
                          std::uint64_t timestamp_ms = 0) {
        if (id == 0 || room_id.empty()) {
            add_notice(std::move(text), timestamp_ms);
            return;
        }
        const bool duplicate = std::any_of(lines.begin(), lines.end(), [&](const auto& line) {
            return line.kind == ChatLineKind::CHAT && line.room_id == room_id && line.message_id == id;
        });
        if (duplicate) {
            return;
        }

        const auto position = std::find_if(lines.begin(), lines.end(), [&](const auto& line) {
            return line.kind == ChatLineKind::CHAT && line.room_id == room_id && line.message_id > id;
        });

        ChatLogLine line;
        line.kind = ChatLineKind::CHAT;
        line.text = std::move(text);
        line.room_id = room_id;
        line.room_name = room_name;
        line.message_id = id;
        line.sender = std::move(sender);
        line.timestamp_ms = timestamp_ms;
        lines.insert(position, std::move(line));
        trim();
    }

    /**
     * @brief 영속 메시지를 중복 없이 ID 순서에 맞게 삽입합니다.
     * @param room_id 영속 방 세대 UUID
     * @param id 방 내 영속 메시지 ID
     * @param text 메시지 본문
     */
    void add_message(const std::string& room_id, std::uint64_t id, std::string text) {
        add_chat_message(room_id, {}, id, {}, std::move(text), 0);
    }

    /**
     * @brief 방 세대와 확정 snapshot 경계를 기록합니다.
     * @param room 표시용 방 이름
     * @param room_id 영속 방 세대 UUID
     * @param watermark DB 스냅샷 시점의 최신 commit ID
     */
    void snapshot(const std::string& room, const std::string& room_id, std::uint64_t watermark) {
        if (room_id.empty()) {
            return;
        }
        auto& previous = rooms_[room];
        if (!previous.empty() && previous != room_id) {
            std::erase_if(lines, [&](const auto& line) {
                return line.room_id == previous;
            });
        }
        previous = room_id;
        watermarks_[room_id] = std::max(watermarks_[room_id], watermark);
    }

    /**
     * @brief 특정 방 이름에 매핑된 최근 영속 `room_id`를 조회합니다.
     * @param room 조회할 표시용 방 이름
     * @return 기록된 `room_id`. 아직 없으면 빈 문자열
     */
    std::string room_id_for(const std::string& room) const {
        if (const auto it = rooms_.find(room); it != rooms_.end()) {
            return it->second;
        }
        return {};
    }

    /**
     * @brief 특정 영속 방 세대의 최대 snapshot watermark를 조회합니다.
     * @param room_id 조회할 영속 방 세대 UUID
     * @return 기록된 최대 watermark. 아직 없으면 0
     */
    std::uint64_t watermark_for(const std::string& room_id) const {
        if (const auto it = watermarks_.find(room_id); it != watermarks_.end()) {
            return it->second;
        }
        return 0;
    }

private:
    void trim() {
        if (lines.size() > 2000) {
            lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(lines.size() - 2000));
        }
    }

    std::unordered_map<std::string, std::string> rooms_;
    std::unordered_map<std::string, std::uint64_t> watermarks_;
};

/**
 * @brief 채팅 및 귓속말 입력에 적용하는 가중 문자 단위 상한입니다.
 *
 * 영문·로마자 계열(`1`단위) 기준 최대 `2,000`자, 한글·기타 유니코드(`2`단위) 기준
 * 최대 `1,000`자까지 입력할 수 있도록 `2,000`단위를 상한으로 사용합니다.
 */
inline constexpr std::size_t kMaxChatCharUnits = 2000;

/**
 * @brief 가중 문자 단위 상한(`kMaxChatCharUnits`)과 Win32 IME 조합 여유분을 수용하는 입력 버퍼 바이트 크기입니다.
 */
inline constexpr std::size_t kChatInputBufferBytes = 4224;

/**
 * @brief UTF-8 문자열의 글자 수와 로마자/유니코드 가중 단위를 요약한 통계입니다.
 */
struct Utf8CharStats {
    std::size_t total_codepoints{};
    std::size_t latin_codepoints{};
    std::size_t unicode_codepoints{};
    std::size_t weighted_units{};
};

/**
 * @brief 유니코드 코드포인트가 로마자 계열(`1`단위)인지 판정합니다.
 * @param codepoint 검사할 유니코드 스칼라 값
 * @return ASCII 및 로마자 계열 문자면 true, 한글·기타 유니코드면 false
 */
inline bool is_latin_char_unit_codepoint(std::uint32_t codepoint) noexcept {
    if (codepoint <= 0x007Fu) {
        return true;
    }
    if (codepoint >= 0x00C0u && codepoint <= 0x024Fu && codepoint != 0x00D7u && codepoint != 0x00F7u) {
        return true;
    }
    return false;
}

/**
 * @brief UTF-8 문자열에서 다음 완전한 코드포인트의 바이트 길이와 가중 단위를 디코드합니다.
 * @param utf8 검사할 UTF-8 문자열 뷰
 * @param offset 시작 바이트 오프셋
 * @param out_bytes 디코드된 UTF-8 시퀀스 바이트 길이를 받을 출력 참조
 * @param out_is_latin 해당 코드포인트가 로마자 계열(`1`단위)인지 여부를 받을 출력 참조
 * @return `offset < utf8.size()`이면 true, 문자열 끝에 도달하면 false
 */
inline bool decode_next_utf8_unit(std::string_view utf8,
                                  std::size_t offset,
                                  std::size_t& out_bytes,
                                  bool& out_is_latin) noexcept {
    if (offset >= utf8.size()) {
        return false;
    }
    const auto b0 = static_cast<unsigned char>(utf8[offset]);
    if (b0 < 0x80u) {
        out_bytes = 1;
        out_is_latin = true;
        return true;
    }

    std::size_t seq_len = 0;
    std::uint32_t cp = 0;
    std::uint32_t min_cp = 0;
    if ((b0 & 0xE0u) == 0xC0u) {
        seq_len = 2;
        cp = static_cast<std::uint32_t>(b0 & 0x1Fu);
        min_cp = 0x80u;
    } else if ((b0 & 0xF0u) == 0xE0u) {
        seq_len = 3;
        cp = static_cast<std::uint32_t>(b0 & 0x0Fu);
        min_cp = 0x800u;
    } else if ((b0 & 0xF8u) == 0xF0u) {
        seq_len = 4;
        cp = static_cast<std::uint32_t>(b0 & 0x07u);
        min_cp = 0x10000u;
    } else {
        out_bytes = 1;
        out_is_latin = false;
        return true;
    }

    if (offset + seq_len > utf8.size()) {
        out_bytes = utf8.size() - offset;
        out_is_latin = false;
        return true;
    }
    for (std::size_t k = 1; k < seq_len; ++k) {
        const auto cont = static_cast<unsigned char>(utf8[offset + k]);
        if ((cont & 0xC0u) != 0x80u) {
            out_bytes = 1;
            out_is_latin = false;
            return true;
        }
        cp = (cp << 6u) | static_cast<std::uint32_t>(cont & 0x3Fu);
    }
    if (cp < min_cp || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
        out_bytes = 1;
        out_is_latin = false;
        return true;
    }

    out_bytes = seq_len;
    out_is_latin = is_latin_char_unit_codepoint(cp);
    return true;
}

/**
 * @brief UTF-8 문자열의 전체 글자 수, 로마자/유니코드 수, 가중 문자 단위를 계산합니다.
 * @param utf8 측정할 UTF-8 문자열
 * @return 코드포인트 및 가중 단위 집계 결과(`Utf8CharStats`)
 */
inline Utf8CharStats measure_utf8_char_stats(std::string_view utf8) noexcept {
    Utf8CharStats stats{};
    std::size_t offset = 0;
    std::size_t bytes = 0;
    bool is_latin = false;
    while (decode_next_utf8_unit(utf8, offset, bytes, is_latin)) {
        offset += bytes;
        ++stats.total_codepoints;
        if (is_latin) {
            ++stats.latin_codepoints;
            stats.weighted_units += 1;
        } else {
            ++stats.unicode_codepoints;
            stats.weighted_units += 2;
        }
    }
    return stats;
}

/**
 * @brief 지정한 가중 문자 단위 상한을 넘지 않는 최대 UTF-8 코드포인트 경계 바이트 길이를 계산합니다.
 * @param utf8 검사할 UTF-8 문자열
 * @param max_units 허용할 최대 가중 문자 단위 수
 * @return 멀티바이트 문자를 자르지 않고 상한 이하를 만족하는 접두사 바이트 길이
 */
inline std::size_t utf8_prefix_bytes_for_char_units(std::string_view utf8,
                                                    std::size_t max_units = kMaxChatCharUnits) noexcept {
    std::size_t offset = 0;
    std::size_t used_units = 0;
    std::size_t bytes = 0;
    bool is_latin = false;
    while (decode_next_utf8_unit(utf8, offset, bytes, is_latin)) {
        const std::size_t cost = is_latin ? 1u : 2u;
        if (used_units + cost > max_units) {
            break;
        }
        used_units += cost;
        offset += bytes;
    }
    return offset;
}

/**
 * @brief UTF-8 문자열을 코드포인트 경계에 맞춰 지정한 가중 문자 단위 상한 이내로 자릅니다.
 * @param utf8 자를 UTF-8 문자열
 * @param max_units 허용할 최대 가중 문자 단위 수
 * @return 가중 문자 단위 상한 이하의 유효한 UTF-8 접두사 문자열
 */
inline std::string truncate_utf8_to_char_units(std::string_view utf8,
                                               std::size_t max_units = kMaxChatCharUnits) {
    const std::size_t prefix_bytes = utf8_prefix_bytes_for_char_units(utf8, max_units);
    return std::string(utf8.substr(0, prefix_bytes));
}

/**
 * @brief 연속 발신자 메시지 그룹화에 사용하는 기본 시간 간격(60초)입니다.
 */
inline constexpr std::uint64_t kChatGroupingWindowMs = 60'000ULL;

/**
 * @brief 직전 방 채팅 항목과 현재 항목을 같은 발신자 그룹으로 묶을 수 있는지 판정합니다.
 * @param prev 바로 앞 타임라인 채팅 항목
 * @param curr 현재 검사할 타임라인 채팅 항목
 * @param max_gap_ms 그룹화를 허용할 최대 시간 간격(ms)
 * @return 같은 방·발신자·유효 시각이면서 간격이 `max_gap_ms` 이하이면 true
 */
inline bool can_group_chat_lines(const ChatLogLine& prev,
                                 const ChatLogLine& curr,
                                 std::uint64_t max_gap_ms = kChatGroupingWindowMs) {
    if (prev.kind != ChatLineKind::CHAT || curr.kind != ChatLineKind::CHAT) {
        return false;
    }
    if (curr.sender.empty() || prev.sender != curr.sender || prev.room_id != curr.room_id) {
        return false;
    }
    if (prev.timestamp_ms == 0 || curr.timestamp_ms < prev.timestamp_ms) {
        return false;
    }
    return (curr.timestamp_ms - prev.timestamp_ms) <= max_gap_ms;
}

/**
 * @brief 타임라인 항목이 특정 상대와의 1:1 귓속말 대화에 속하는지 확인합니다.
 * @param line 검사할 타임라인 항목
 * @param peer 비교할 1:1 귓속말 상대 식별자
 * @return 해당 상대와의 수신/발신 귓속말이면 true
 */
inline bool matches_whisper_peer(const ChatLogLine& line, std::string_view peer) {
    if (peer.empty()) {
        return false;
    }
    if (line.kind == ChatLineKind::WHISPER_IN) {
        return line.sender == peer;
    }
    if (line.kind == ChatLineKind::WHISPER_OUT) {
        return line.recipient == peer;
    }
    return false;
}

/**
 * @brief UTF-8 문자열에서 ASCII 영문 대소문자를 구분하지 않고 부분 일치 검색을 수행합니다.
 * @param line 검사할 타임라인 항목
 * @param query 검색어 문자열
 * @return 검색어가 비어 있거나 본문·발신자·수신자·방 이름 중 하나에 일치하면 true
 */
inline bool matches_timeline_search(const ChatLogLine& line, std::string_view query) {
    if (query.empty()) {
        return true;
    }
    const auto ascii_ci_contains = [](std::string_view haystack, std::string_view needle) {
        const auto fold = [](unsigned char c) -> unsigned char {
            return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
        };
        const auto it = std::search(
            haystack.begin(),
            haystack.end(),
            needle.begin(),
            needle.end(),
            [&](char a, char b) {
                return fold(static_cast<unsigned char>(a)) == fold(static_cast<unsigned char>(b));
            });
        return it != haystack.end();
    };
    return ascii_ci_contains(line.text, query) ||
           ascii_ci_contains(line.sender, query) ||
           ascii_ci_contains(line.recipient, query) ||
           ascii_ci_contains(line.room_name, query);
}

/**
 * @brief 채팅 및 슬래시 명령 입력창의 방향키(`Up`/`Down`) 탐색용 히스토리를 관리합니다.
 *
 * 연속 중복 입력은 하나로 합치고 탐색 시작 전 작성 중이던 임시 초안(`draft`)을 보존해
 * 맨 아래로 돌아왔을 때 원래 입력 내용을 복원합니다.
 */
class InputHistory {
public:
    /**
     * @brief 지정한 최대 보관 개수로 입력 히스토리를 초기화합니다.
     * @param max_entries 보관할 최대 입력 개수
     */
    explicit InputHistory(std::size_t max_entries = 50)
        : max_entries_(max_entries) {}

    /**
     * @brief 전송된 입력 문자열을 히스토리에 기록하고 탐색 커서를 초기화합니다.
     * @param entry 전송 완료된 메시지 또는 명령 문자열
     */
    void record(std::string_view entry) {
        if (entry.empty()) {
            return;
        }
        if (max_entries_ > 0 && (entries_.empty() || entries_.back() != entry)) {
            entries_.emplace_back(entry);
            if (entries_.size() > max_entries_) {
                entries_.erase(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(entries_.size() - max_entries_));
            }
        }
        reset_navigation();
    }

    /**
     * @brief 이전(`Up`) 히스토리 항목을 반환합니다.
     * @param current_input 탐색을 시작하는 시점의 현재 입력창 내용
     * @return 입력창에 표시할 이전 항목(히스토리가 비어 있으면 `current_input`)
     */
    std::string navigate_up(std::string_view current_input) {
        if (entries_.empty()) {
            return std::string(current_input);
        }
        if (cursor_ < 0) {
            draft_.assign(current_input);
            cursor_ = static_cast<int>(entries_.size()) - 1;
        } else {
            cursor_ = std::max(0, cursor_ - 1);
        }
        return entries_[static_cast<std::size_t>(cursor_)];
    }

    /**
     * @brief 다음(`Down`) 히스토리 항목 또는 저장해 둔 작성 초안을 반환합니다.
     * @param current_input 현재 입력창 내용
     * @return 다음 히스토리 항목, 끝을 지나면 초기 초안(`draft`)
     */
    std::string navigate_down(std::string_view current_input) {
        if (cursor_ < 0) {
            return std::string(current_input);
        }
        ++cursor_;
        if (cursor_ >= static_cast<int>(entries_.size())) {
            cursor_ = -1;
            std::string restored = std::move(draft_);
            draft_.clear();
            return restored;
        }
        return entries_[static_cast<std::size_t>(cursor_)];
    }

    /**
     * @brief 히스토리 탐색 상태와 임시 초안을 초기화합니다.
     */
    void reset_navigation() {
        cursor_ = -1;
        draft_.clear();
    }

    /**
     * @brief 보관 중인 히스토리 개수를 반환합니다.
     * @return 저장된 입력 항목 수
     */
    std::size_t size() const { return entries_.size(); }

    /**
     * @brief 현재 히스토리 탐색 중인지 여부를 반환합니다.
     * @return 탐색 중이면 true, 새 입력 상태면 false
     */
    bool navigating() const { return cursor_ >= 0; }

private:
    std::size_t max_entries_{50};
    std::vector<std::string> entries_;
    std::string draft_;
    int cursor_{-1};
};
