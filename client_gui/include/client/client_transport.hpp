#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

/**
 * @brief 클라이언트 전송 계층의 연결 상태입니다.
 *
 * UI와 애플리케이션 계층이 동기/비동기 연결 진행, 활성 연결, 종료 상태를 구분해
 * 강제 차단 모달 없이 상태 배지와 재접속 버튼을 제어할 수 있도록 제공합니다.
 */
enum class TransportState {
    DISCONNECTED,
    CONNECTING,
    CONNECTED
};

/**
 * @brief 서버 `LoginRes`에서 수신한 인증 및 세션 연속성 원시 정보입니다.
 *
 * `resume_token` 원문은 전송 계층과 `ClientApp` 내부 재접속 조합에만 사용하며,
 * UI 표시 모델이나 로그로 직접 노출하지 않습니다.
 */
struct TransportSessionInfo {
    std::string principal_id;
    std::string auth_session_id;
    std::string display_name;
    std::string logical_session_id;
    std::string resume_token;
    std::uint64_t resume_expires_unix_ms{};
    bool resumed{};
};

/**
 * @brief 방 스냅샷에 포함된 개별 영속 채팅 메시지 항목입니다.
 */
struct SnapshotMessage {
    std::string sender;
    std::string text;
    std::uint64_t ts_ms{};
    std::uint64_t id{};
};

/**
 * @brief `ClientApp`이 네트워크 I/O 구현과 분리되기 위한 추상 전송 인터페이스입니다.
 *
 * 실제 실행에서는 `NetClient`가 이를 구현하고, 단위 테스트에서는 가짜(Fake) 전송 구현을
 * 주입해 Boost.Asio 소켓 없이도 로그인·토큰 비노출·명령 라우팅·RTT 반영을 검증합니다.
 */
class IClientTransport {
public:
    /** @brief 서버 `MSG_HELLO` 수신 콜백 시그니처입니다. */
    using OnHello = std::function<void(std::uint16_t caps)>;
    /** @brief 서버 `MSG_ERR` 수신 콜백 시그니처입니다. */
    using OnErr = std::function<void(std::uint16_t code, std::string msg)>;
    /** @brief 로그인 응답(`MSG_LOGIN_RES`) 콜백 시그니처입니다. */
    using OnLoginRes = std::function<void(std::string effective_user, std::uint32_t sid, bool is_admin)>;
    /** @brief 인증·세션 연속성 상세 정보 콜백 시그니처입니다. */
    using OnSessionInfo = std::function<void(TransportSessionInfo info)>;
    /** @brief 채팅/시스템 브로드캐스트(`MSG_CHAT_BROADCAST`) 콜백 시그니처입니다. */
    using OnBroadcast = std::function<void(std::string room,
                                           std::string sender,
                                           std::string text,
                                           std::uint16_t flags,
                                           std::uint32_t sender_sid,
                                           std::uint64_t message_id,
                                           std::string room_id,
                                           std::uint64_t ts_ms)>;
    /** @brief 방 참여자 목록(`MSG_ROOM_USERS`) 콜백 시그니처입니다. */
    using OnRoomUsers = std::function<void(std::string room, std::vector<std::string> users)>;
    /** @brief 상태 스냅샷(`MSG_STATE_SNAPSHOT`) 콜백 시그니처입니다. */
    using OnSnapshot = std::function<void(std::string current,
                                          std::vector<std::string> rooms,
                                          std::vector<std::string> users,
                                          std::vector<bool> locked,
                                          std::vector<SnapshotMessage> messages,
                                          std::string your_name,
                                          std::uint64_t watermark,
                                          std::string room_id,
                                          bool history_available)>;
    /** @brief 귓속말 수신/발신(`MSG_WHISPER_BROADCAST`) 콜백 시그니처입니다. */
    using OnWhisper = std::function<void(std::string sender, std::string recipient, std::string text, bool outgoing)>;
    /** @brief 귓속말 처리 결과(`MSG_WHISPER_RES`) 콜백 시그니처입니다. */
    using OnWhisperResult = std::function<void(bool ok, std::string reason)>;
    /** @brief 서버 상태 갱신 알림(`MSG_REFRESH_NOTIFY`) 콜백 시그니처입니다. */
    using OnRefreshNotify = std::function<void()>;
    /** @brief 연결 종료 콜백 시그니처입니다. */
    using OnDisconnected = std::function<void(std::string reason)>;
    /** @brief 연결 상태 전이 콜백 시그니처입니다. */
    using OnStateChanged = std::function<void(TransportState state)>;
    /** @brief `MSG_PONG` RTT 측정 완료 콜백 시그니처입니다. */
    using OnPong = std::function<void(std::uint32_t rtt_ms)>;

    /** @brief 다형성 소멸을 보장하는 기본 가상 소멸자입니다. */
    virtual ~IClientTransport() = default;

    /**
     * @brief `MSG_HELLO` 콜백을 등록합니다.
     * @param f 서버 capability 비트를 전달받을 콜백
     */
    virtual void set_on_hello(OnHello f) = 0;

    /**
     * @brief `MSG_ERR` 콜백을 등록합니다.
     * @param f 오류 코드와 메시지를 전달받을 콜백
     */
    virtual void set_on_err(OnErr f) = 0;

    /**
     * @brief 로그인 응답 콜백을 등록합니다.
     * @param f 확정 사용자명, 세션 ID, 관리자 여부를 전달받을 콜백
     */
    virtual void set_on_login_res(OnLoginRes f) = 0;

    /**
     * @brief 세션 연속성 메타데이터 콜백을 등록합니다.
     * @param f `TransportSessionInfo`를 전달받을 콜백
     */
    virtual void set_on_session_info(OnSessionInfo f) = 0;

    /**
     * @brief 채팅 및 시스템 브로드캐스트 콜백을 등록합니다.
     * @param f 브로드캐스트 페이로드 필드를 전달받을 콜백
     */
    virtual void set_on_broadcast(OnBroadcast f) = 0;

    /**
     * @brief 방 참여자 목록 응답 콜백을 등록합니다.
     * @param f 방 이름과 참여자 목록을 전달받을 콜백
     */
    virtual void set_on_room_users(OnRoomUsers f) = 0;

    /**
     * @brief 상태 스냅샷 콜백을 등록합니다.
     * @param f 현재 방/목록/메시지 창/watermark 정보를 전달받을 콜백
     */
    virtual void set_on_snapshot(OnSnapshot f) = 0;

    /**
     * @brief 귓속말 메시지 콜백을 등록합니다.
     * @param f 귓속말 발신자, 수신자, 본문, 발신 여부를 전달받을 콜백
     */
    virtual void set_on_whisper(OnWhisper f) = 0;

    /**
     * @brief 귓속말 결과 콜백을 등록합니다.
     * @param f 성공 여부와 실패 사유를 전달받을 콜백
     */
    virtual void set_on_whisper_result(OnWhisperResult f) = 0;

    /**
     * @brief 상태 갱신 알림 콜백을 등록합니다.
     * @param f `MSG_REFRESH_NOTIFY` 수신 시 호출할 콜백
     */
    virtual void set_on_refresh_notify(OnRefreshNotify f) = 0;

    /**
     * @brief 연결 종료 알림 콜백을 등록합니다.
     * @param f 종료 사유 문자열을 전달받을 콜백
     */
    virtual void set_on_disconnected(OnDisconnected f) = 0;

    /**
     * @brief 연결 상태 변경 알림 콜백을 등록합니다.
     * @param f 변경된 `TransportState`를 전달받을 콜백
     */
    virtual void set_on_state_changed(OnStateChanged f) = 0;

    /**
     * @brief `MSG_PONG` RTT 측정 콜백을 등록합니다.
     * @param f 왕복 지연(ms)을 전달받을 콜백
     */
    virtual void set_on_pong(OnPong f) = 0;

    /**
     * @brief 지정한 호스트와 포트로 서버 연결을 수립합니다.
     * @param host 대상 호스트명 또는 IP 주소
     * @param port 대상 TCP 포트 번호
     * @return 연결 수립 및 수신 루프 시작에 성공하면 true, 실패하면 false
     */
    virtual bool connect(const std::string& host, unsigned short port) = 0;

    /**
     * @brief 현재 연결과 백그라운드 I/O 루프를 정리하고 종료합니다.
     */
    virtual void close() = 0;

    /**
     * @brief 현재 전송 계층이 연결 상태인지 반환합니다.
     * @return 활성 TCP 연결이 유지 중이면 true
     */
    virtual bool is_connected() const = 0;

    /**
     * @brief 로그인 요청(`MSG_LOGIN_REQ`)을 전송합니다.
     * @param user 요청 닉네임 또는 식별자
     * @param token access 또는 `access|resume:...` 자격 문자열
     */
    virtual void send_login(const std::string& user, const std::string& token) = 0;

    /**
     * @brief 방 입장 또는 생성 요청(`MSG_JOIN_ROOM`)을 전송합니다.
     * @param room 대상 방 이름
     * @param password 잠금방 비밀번호(공개방이면 빈 문자열)
     */
    virtual void send_join(const std::string& room, const std::string& password = std::string()) = 0;

    /**
     * @brief 방 퇴장 요청(`MSG_LEAVE_ROOM`)을 전송합니다.
     * @param room 나갈 방 이름
     */
    virtual void send_leave(const std::string& room = std::string()) = 0;

    /**
     * @brief 채팅 메시지 또는 서버 슬래시 명령(`MSG_CHAT_SEND`)을 전송합니다.
     * @param room 현재 방 이름
     * @param text 본문 또는 슬래시 명령 문자열
     */
    virtual void send_chat(const std::string& room, const std::string& text) = 0;

    /**
     * @brief 현재 방 스냅샷 재동기화 요청(`MSG_REFRESH_REQ`)을 전송합니다.
     * @param current_room 현재 방 이름
     */
    virtual void send_refresh(const std::string& current_room) = 0;

    /**
     * @brief 특정 방 참여자 목록 요청(`MSG_ROOM_USERS_REQ`)을 전송합니다.
     * @param room 조회할 방 이름
     */
    virtual void send_who(const std::string& room) = 0;

    /**
     * @brief 방 목록 요청(`MSG_ROOMS_REQ`)을 전송합니다.
     * @param current_room 현재 방 이름
     */
    virtual void send_rooms(const std::string& current_room) = 0;

    /**
     * @brief 귓속말 요청(`MSG_WHISPER_REQ`)을 전송합니다.
     * @param user 대상 사용자 식별자
     * @param text 귓속말 내용
     */
    virtual void send_whisper(const std::string& user, const std::string& text) = 0;

    /**
     * @brief 왕복 지연 측정을 포함한 keepalive `MSG_PING`을 전송합니다.
     */
    virtual void send_ping() = 0;

    /**
     * @brief 주기적 자동 `MSG_PING` 하트비트 송신 여부를 설정합니다.
     * @param enabled 주기적 자동 `MSG_PING`을 활성화하면 true
     */
    virtual void set_auto_ping_enabled(bool enabled) { (void)enabled; }
};
