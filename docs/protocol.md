# 프로토콜 설계

## 목표
- 간단하고 견고한 바이너리 프로토콜로 시작해 확장이 쉽도록 한다.
- 외부(클라이언트↔Gateway)와 내부(서비스 간) 프로토콜을 분리한다.
- 단일 소스: 패킷 헤더/프레이밍의 표준 구현은 `core/include/server/core/protocol/frame.hpp`로 제공하며, 코어를 사용하는 모든 프로그램은 이를 공유한다.

## 패킷 형식
- Endianness: big-endian(네트워크 바이트 오더).
- Header 14바이트(고정, v1.1):
  - `uint16 length`: 본문(payload) 길이
  - `uint16 msg_id`: 메시지 식별자
  - `uint16 flags`: 옵션 비트(압축/암호화 등)
  - `uint32 seq`: 발신 측 시퀀스(단조 증가, wrap 허용)
  - `uint32 utc_ts_ms32`: 발신 시점 UTC epoch ms 하위 32비트
- Body: `length` 바이트.

### Flags 비트 정의(초안)
- bit0: COMPRESSED(LZ4)
- bit1: ENCRYPTED(TLS 외 경량 암호화 사용 시)
- bit2~15: 예약(SEQ/TS는 고정 헤더 포함이므로 플래그 불요)

## 예약 메시지 ID
- 소스 오브 트루스: `protocol/opcodes.json` (빌드 시 `protocol.hpp` 자동 생성)
- `0x0001` MSG_HELLO: 버전/서버 정보 안내(서버→클라).
- `0x0002` MSG_PING / `0x0003` MSG_PONG: heartbeat.
- `0x0004` MSG_ERR: 에러 응답(코드/메시지 포함).
- `0x0010` MSG_LOGIN_REQ / `0x0011` MSG_LOGIN_RES: 인증(샘플).
- `0x0100` MSG_CHAT_SEND / `0x0101` MSG_CHAT_BROADCAST: 채팅(샘플).
- `0x0200` MSG_STATE_SNAPSHOT: 상태 스냅샷(방 목록+현재 방 유저)
- `0x0201` MSG_ROOM_USERS: 특정 방 유저 목록 응답

## 길이 제한/보안
- `length` 상한: 기본 32KB(설정 가능). 초과 시 세션 종료.
- 압축: `flags`에 LZ4 표시, 임계치(예: 1KB) 초과 시 적용.
- 암호화: TLS 권장. 경량 XOR/ChaCha20은 개발·테스트용.

## 시퀀스/재전송
- `seq`와 `utc_ts_ms32`는 항상 고정 헤더에 포함된다.
- 현재는 재전송/ACK 등의 신뢰 레이어를 구현하지 않으며, `seq`는 클라이언트/서버 측 추적/디버깅/순서 확인 용도로만 사용한다.

## 예시 인코딩
```
[00 0A] [01 00] [00 00] [00 00 00 2A] [00 00 01 89] | payload(10B)
 length  msg_id   flags     seq(uint32)      utc_ts_ms32
```

## 호환성 전략
- msg_id 예약 구간을 모듈별로 분배(예: 0x0000~0x0FFF: 코어, 0x1000~: 게임).
- 스키마 변경 시: 버전 필드 추가 또는 새로운 msg_id 도입으로 하위 호환 유지.

---

## 내부 프로토콜(서비스 간)

### RPC(gRPC)
- 권장: Protobuf IDL. 예시 서비스:
  - Auth: `Login`, `ValidateToken`, `RefreshToken`.
  - Chat: `JoinRoom`, `LeaveRoom`, `SendMessage`, `ListRooms`.
  - Presence: `SetOnline`, `SetOffline`, `GetPresence`.
- 공통 메타데이터: `trace_id`, `correlation_id`, `tenant`, `user_id`.

### 이벤트(Event Bus)
- 주제(topic) 설계: `<service>.events.<noun>`.
- 메시지 예시: `chat.events.message` { room_id, sender_id, text, ts, correlation_id }.
- 멱등성 키: `idempotency_key`로 중복 처리 방지.

### 보안
- mTLS + 서비스 계정. JWT는 헤더에 전파, 서비스는 역할 기반 접근제어(RBAC).

---

## 외부 핸드셰이크/헬로(권장)
- 흐름: Client 접속 → 서버가 `MSG_HELLO` 전송 → 클라가 선택적 기능 협상 후 로그인 시도.
- `MSG_HELLO` payload(v1.1):
  - `uint16 proto_major`, `uint16 proto_minor`
- `uint16 capabilities` 비트필드(예: CAP_COMPRESS_SUPP, CAP_SENDER_SID)
  - `uint16 heartbeat_interval_x10ms`(10ms 단위 인터벌 값)
  - `uint32 epoch_high32`(서버 UTC ms 상위 32비트; 클라가 수신 헤더의 `utc_ts_ms32`와 결합해 64비트 시각 재구성 가능)


  - 0x0007: INVALID_PAYLOAD
  - 0x0100: NAME_TAKEN(닉네임 중복)
  - 필요 시 확장: MALFORMED_FRAME, FORBIDDEN, RATE_LIMITED, INTERNAL_ERROR 등은 도입 시 코드 예약 후 사용

## 문자열/인코딩 규칙(중요)
- 모든 문자열은 UTF-8로 인코딩한다(서버/클라 모두).
- 문자열 필드는 길이-접두(prefix) 방식 사용을 권장: `uint16 len` + `len` 바이트(UTF-8). 널 종료는 사용하지 않는다.

## 메시지 payload 스키마(발췌)
- `MSG_LOGIN_REQ`: `u16 len_user | user(utf8)` + `u16 len_token | token(utf8)`
- `MSG_LOGIN_RES`: `u8 status(0:ok,1:fail)` + `u16 len_msg | msg(utf8)` + `[u16 len_user | effective_user(utf8)]` + `[u32 session_id]`
  - 비어 있거나 `guest`로 로그인 시 서버가 8자리 16진 임시 닉네임을 부여하고 `effective_user`로 반환
- `MSG_CHAT_SEND`: `u16 len_room | room(utf8)` + `u16 len_text | text(utf8)`
- `MSG_CHAT_BROADCAST`: `u16 len_room | room(utf8)` + `u16 len_sender | sender(utf8)` + `u16 len_text | text(utf8)` + `[u32 sender_sid]` + `u64 ts_ms`
  - `sender_sid`는 `HELLO.capabilities`에 `CAP_SENDER_SID`가 포함될 때 채워짐(이전 클라 하위 호환성 위해 생략 가능)
- `MSG_STATE_SNAPSHOT`: `u16 len_current | current_room(utf8)` + `u16 rooms_count` + `[u16 len_room | room][u16 members]*...` + `u16 users_count` + `[u16 len_user | user]*...`
- `MSG_ROOM_USERS`: `u16 len_room | room(utf8)` + `u16 users_count` + `[u16 len_user | user]*...`

## 호환성/버전 정책
- `proto_major`가 다르면 접속 거부 또는 다운그레이드 불가 응답.
- `proto_minor`는 하위 호환 유지. 새로운 기능은 capability로 협상.

## 라우팅 규칙(게이트웨이 관점, 요약)
- Pre-Auth 허용 메시지: `MSG_HELLO`, `MSG_LOGIN_REQ`, `MSG_PING`.
- Post-Auth에서만 허용: 채팅/룸 조작, 기타 도메인 메시지.
- 위반 시 `MSG_ERR(UNAUTHORIZED)` 전송 후 정책에 따라 세션 종료.

---

## 변경 이력(요약)
- v1.1
  - 고정 헤더: 14바이트(변경 없음).
  - `MSG_HELLO` payload를 12바이트로 명시: `u16 major`, `u16 minor`, `u16 capabilities`, `u16 heartbeat_x10ms`, `u32 epoch_high32`.
  - 서버 구현에서 과거 10바이트로 인코딩하던 버그를 수정(힙 오염/오버리드 예방).
- `MSG_CHAT_BROADCAST`에 `u32 sender_sid`(cap 지원 시) + `u64 ts_ms` 포함.
- `MSG_STATE_SNAPSHOT(0x0200)`/`MSG_ROOM_USERS(0x0201)` 도입.

