#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "client/client_transport.hpp"
#include "client/message_timeline.hpp"

/**
 * @brief 서버에서 받은 방 목록의 개별 항목 정보입니다.
 */
struct RoomInfo {
    std::string name;
    bool locked{};
};

/**
 * @brief UI와 진단 패널에 안전하게 노출할 수 있는 세션 요약 정보입니다.
 *
 * `access_token`이나 `resume_token` 원문은 포함하지 않고 보유 여부와 만료 시각만 노출합니다.
 */
struct SessionSummary {
    std::string effective_user;
    std::uint32_t session_id{};
    std::string principal_id;
    std::string auth_session_id;
    std::string display_name;
    std::string logical_session_id;
    std::uint64_t resume_expires_unix_ms{};
    bool resumed{};
    bool has_resume_token{};
};

/**
 * @brief 프로토콜 이벤트 로그 링 버퍼가 보관하는 최대 항목 수입니다.
 */
inline constexpr std::size_t kMaxProtocolEvents = 250;

/**
 * @brief 선택형 `Auto-Resume` 지수 백오프 재시도의 최대 시도 횟수입니다.
 */
inline constexpr std::uint32_t kAutoResumeMaxAttempts = 5;

/**
 * @brief 프로토콜 이벤트 로그의 송수신 방향 구분입니다.
 */
enum class ProtocolDirection {
    TX,
    RX,
    LIFECYCLE
};

/**
 * @brief `Workbench` 모드의 프로토콜 이벤트 로그 탭에 기록되는 요약 항목입니다.
 *
 * `access_token`, `resume_token`, 방 비밀번호 등 민감한 원문 문자열은 절대 기록하지 않습니다.
 */
struct ProtocolEventEntry {
    std::uint64_t timestamp_ms{};
    ProtocolDirection direction{ProtocolDirection::LIFECYCLE};
    std::string opcode;
    std::string summary;
};

/**
 * @brief 개발 워크벤치 모드에서 표시하는 연결·프로토콜·스냅샷 진단 상태입니다.
 */
struct DiagnosticsSummary {
    std::uint16_t server_caps{};
    std::uint32_t last_rtt_ms{};
    std::uint32_t min_rtt_ms{};
    std::uint32_t max_rtt_ms{};
    std::uint32_t avg_rtt_ms{};
    std::uint32_t rtt_sample_count{};
    bool has_rtt{};
    bool auto_ping_enabled{true};
    bool auto_resume_enabled{false};
    bool auto_resume_pending{false};
    std::uint32_t auto_resume_attempt{};
    std::uint64_t auto_resume_next_unix_ms{};
    std::string current_room_id;
    std::uint64_t current_watermark{};
    bool history_available{true};
    std::uint16_t last_error_code{};
    std::string last_error_message;
    std::string last_disconnect_reason;
};

/**
 * @brief UI 계층이 읽기 전용으로 참조하는 클라이언트 애플리케이션 상태 스냅샷입니다.
 */
struct AppData {
    TransportState connection_state{TransportState::DISCONNECTED};
    bool is_connected{false};
    bool is_logged_in{false};
    bool is_admin{false};
    std::string last_host{"127.0.0.1"};
    int last_port{6000};
    SessionSummary session;
    DiagnosticsSummary diagnostics;

    std::string current_room{"lobby"};
    std::vector<std::string> user_list;
    std::vector<RoomInfo> room_list;
    std::vector<std::string> whisper_peers;
    std::vector<std::string> blocked_users;
    std::unordered_map<std::string, std::uint32_t> whisper_unread_by_peer;
    std::uint32_t total_unread_whispers{0};

    MessageTimeline timeline;
    std::deque<ProtocolEventEntry> protocol_events;

    /**
     * @brief 타임라인에 시스템 안내 항목을 추가합니다.
     * @param msg 표시할 안내 문자열
     */
    void add_log(const std::string& msg) {
        timeline.add_notice(msg);
    }

    /**
     * @brief 타임라인의 마지막 메시지 문자열을 반환합니다.
     * @return 마지막 항목의 `text`. 비어 있으면 빈 문자열
     */
    std::string get_last_log() const {
        if (timeline.lines.empty()) {
            return "";
        }
        return timeline.lines.back().text;
    }
};

/**
 * @brief 백그라운드 네트워크 콜백을 UI/메인 스레드로 전달하는 스레드 안전 이벤트 큐입니다.
 */
class EventQueue {
public:
    /**
     * @brief 메인 스레드에서 실행할 상태 갱신 작업을 큐에 추가합니다.
     * @param task 실행할 작업 함수 객체
     */
    void push(std::function<void()> task) {
        std::lock_guard<std::mutex> lock(mu_);
        tasks_.push_back(std::move(task));
    }

    /**
     * @brief 현재까지 큐에 쌓인 모든 작업을 꺼내 순차적으로 실행합니다.
     */
    void process_all() {
        std::vector<std::function<void()>> current_batch;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (tasks_.empty()) {
                return;
            }
            while (!tasks_.empty()) {
                current_batch.push_back(std::move(tasks_.front()));
                tasks_.pop_front();
            }
        }
        for (auto& task : current_batch) {
            task();
        }
    }

private:
    std::deque<std::function<void()>> tasks_;
    std::mutex mu_;
};

/**
 * @brief UI 비의존 클라이언트 상태 관리 및 명령 라우팅 컨트롤러입니다.
 *
 * `IClientTransport`를 주입받아 네트워크 I/O 구현과 분리되며,
 * 메신저 모드와 워크벤치 모드가 동일한 데이터 계약(`AppData`)을 공유하도록 유지합니다.
 */
class ClientApp {
public:
    /**
     * @brief 네트워크 전송 인터페이스를 주입받아 `ClientApp`을 초기화합니다.
     * @param transport `ClientApp` 수명 동안 유효한 전송 계층 구현체 참조
     */
    explicit ClientApp(IClientTransport& transport);

    /** @brief 활성 전송 연결을 정리하며 컨트롤러를 소멸합니다. */
    ~ClientApp();

    /**
     * @brief 대기 중인 네트워크 이벤트를 반영하고 예약된 `Auto-Resume` 재시도를 처리합니다.
     * @param now_unix_ms_override 테스트에서 결정적 시각을 주입할 UNIX 밀리초(`0`이면 현재 월클록 사용)
     */
    void update(std::uint64_t now_unix_ms_override = 0);

    /**
     * @brief 지정한 호스트와 포트로 연결을 시도합니다.
     * @param host 서버 호스트 또는 IP 주소
     * @param port 서버 TCP 포트 번호 (1..65535)
     * @return 연결 수립에 성공하면 true, 실패하면 false
     */
    bool connect(const std::string& host, int port);

    /**
     * @brief 현재 서버 연결을 종료하고 세션 활성 플래그를 정리합니다.
     */
    void disconnect();

    /**
     * @brief 로그인 요청을 전송합니다.
     * @param username 요청 사용자명
     * @param access_token signed 모드 발급 자격(개발 모드면 빈 문자열)
     * @param resume 보관된 `resume_token`을 함께 전송해 연속성 복원을 요청할지 여부
     */
    void login(const std::string& username, const std::string& access_token = {}, bool resume = false);

    /**
     * @brief 기존 연결을 정리한 뒤 재연결하고 즉시 로그인(또는 resume)을 요청합니다.
     * @param host 대상 호스트 주소
     * @param port 대상 포트 번호
     * @param username 요청 사용자명
     * @param access_token signed 모드 발급 자격(선택)
     * @param resume 이전 세션 복원 자격 사용 여부
     * @return TCP 연결 수립에 성공하면 true
     */
    bool reconnect_and_login(const std::string& host,
                             int port,
                             const std::string& username,
                             const std::string& access_token = {},
                             bool resume = false);

    /**
     * @brief 사용자 채팅 입력 또는 슬래시 명령어를 해석해 전송합니다.
     * @param input 입력창 문자열
     */
    void process_command(const std::string& input);

    /**
     * @brief 방에 입장하거나 새 방을 생성합니다.
     * @param room_name 대상 방 이름
     * @param password 잠금방 비밀번호(공개방이면 빈 문자열)
     */
    void join_room(const std::string& room_name, const std::string& password = "");

    /**
     * @brief 현재 방에서 나가 로비로 돌아갑니다.
     */
    void leave_current_room();

    /**
     * @brief 특정 사용자에게 귓속말을 전송합니다.
     * @param target 대상 사용자 식별자
     * @param message 귓속말 본문
     */
    void whisper(const std::string& target, const std::string& message);

    /**
     * @brief 지정 사용자를 개인 차단 목록에 추가 요청합니다.
     * @param target 차단할 사용자 식별자
     */
    void block_user(const std::string& target);

    /**
     * @brief 지정 사용자를 개인 차단 목록에서 해제 요청합니다.
     * @param target 차단을 해제할 사용자 식별자
     */
    void unblock_user(const std::string& target);

    /**
     * @brief 서버에 개인 차단 목록(`/blacklist list`) 동기화를 요청합니다.
     */
    void request_blacklist();

    /**
     * @brief 로컬 캐시된 차단 목록에 해당 사용자가 포함되어 있는지 확인합니다.
     * @param target 확인할 사용자 식별자
     * @return 차단 목록에 포함되어 있으면 true
     */
    bool is_user_blocked(const std::string& target) const;

    /**
     * @brief 현재 방 스냅샷 재요청(`MSG_REFRESH_REQ`)을 전송합니다.
     */
    void request_refresh();

    /**
     * @brief 지연 측정을 위한 수동 `MSG_PING`을 전송합니다.
     */
    void request_ping();

    /**
     * @brief 현재 보관된 유효한 `resume_token`으로 세션 복원을 요청할 수 있는지 반환합니다.
     * @return 비어 있지 않은 `resume_token`을 보유 중이면 true
     */
    bool can_resume() const;

    /**
     * @brief 메모리에 보관 중인 `resume_token` 자격을 폐기합니다.
     */
    void clear_saved_resume_token();

    /**
     * @brief 예기치 않은 연결 종료 시 지수 백오프 자동 `Resume` 재연결 사용 여부를 설정합니다.
     * @param enabled 자동 `Resume` 재연결을 활성화하면 true, 끄고 예약 재시도를 취소하려면 false
     */
    void set_auto_resume_enabled(bool enabled);

    /**
     * @brief 선택형 `Auto-Resume` 활성화 여부를 반환합니다.
     * @return 자동 `Resume`이 활성화되어 있으면 true
     */
    bool auto_resume_enabled() const { return data_.diagnostics.auto_resume_enabled; }

    /**
     * @brief 예약된 `Auto-Resume` 재연결 대기와 시도 카운터를 즉시 취소합니다.
     */
    void cancel_auto_resume();

    /**
     * @brief 주기적 자동 `MSG_PING` 하트비트 전송 여부를 설정합니다.
     * @param enabled 자동 `MSG_PING`을 활성화하면 true
     */
    void set_auto_ping_enabled(bool enabled);

    /**
     * @brief 주기적 자동 `MSG_PING` 활성화 여부를 반환합니다.
     * @return 자동 `MSG_PING`이 활성화되어 있으면 true
     */
    bool auto_ping_enabled() const { return data_.diagnostics.auto_ping_enabled; }

    /**
     * @brief `Workbench` 프로토콜 이벤트 로그 링 버퍼를 비웁니다.
     */
    void clear_protocol_events();

    /**
     * @brief 특정 상대와의 미읽음 귓속말 카운터를 읽음 처리합니다.
     * @param peer 읽음 처리할 귓속말 상대 식별자
     */
    void mark_whisper_peer_read(const std::string& peer);

    /**
     * @brief 모든 상대의 미읽음 귓속말 카운터를 0으로 초기화합니다.
     */
    void mark_all_whispers_read();

    /**
     * @brief 특정 상대와의 미읽음 귓속말 개수를 조회합니다.
     * @param peer 조회할 귓속말 상대 식별자
     * @return 해당 상대의 미읽음 귓속말 개수
     */
    std::uint32_t unread_whisper_count(const std::string& peer) const;

    /**
     * @brief 현재 애플리케이션 상태 스냅샷을 읽기 전용으로 반환합니다.
     * @return `AppData` 상수 참조
     */
    const AppData& get_data() const { return data_; }

private:
    void setup_callbacks();
    void sync_blacklist_from_notice(const std::string& text);
    void record_whisper_peer(const std::string& peer);
    void record_rtt_sample(std::uint32_t rtt_ms);
    void reset_rtt_stats();
    void schedule_auto_resume_retry(std::uint64_t now_ms, const std::string& reason);
    void record_protocol_event(ProtocolDirection direction,
                               std::string opcode,
                               std::string summary,
                               std::uint64_t ts_ms = 0);
    std::uint64_t current_now_ms() const;

    IClientTransport& transport_;
    AppData data_;
    EventQueue event_queue_;
    std::string resume_token_secret_;
    std::string last_login_user_;
    std::string last_access_token_;
    std::deque<std::uint32_t> rtt_samples_;
    std::uint64_t active_now_ms_{0};
    bool has_snapshot_room_{false};
    bool manual_disconnect_{false};
};
