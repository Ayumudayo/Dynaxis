# 프로토콜 설계

## 목표
- 간단하고 견고한 바이너리 프로토콜로 시작해 확장이 쉽도록 한다.
- 외부(클라이언트↔Gateway)와 내부(서비스 간) 프로토콜을 분리한다.
- 단일 소스: 패킷 헤더/프레이밍의 표준 구현은 `core/include/server/core/protocol/packet.hpp`로 제공하며, 코어를 사용하는 모든 프로그램은 이를 공유한다. (`core/include/server/core/protocol/packet.hpp`)

## 패킷 형식
- Endianness: big-endian(네트워크 바이트 오더).
- Header 14바이트(고정, v1.1): (`core/include/server/core/protocol/packet.hpp`)
  - `uint16 length`: 본문(payload) 길이 (`core/include/server/core/protocol/packet.hpp`)
  - `uint16 msg_id`: 메시지 식별자 (`core/include/server/core/protocol/packet.hpp`)
  - `uint16 flags`: 옵션 비트(압축/암호화 등) (core/include/server/core/protocol/protocol_flags.hpp:8)
  - `uint32 seq`: 발신 측 시퀀스(단조 증가, wrap 허용) (`core/include/server/core/protocol/packet.hpp`)
  - `uint32 utc_ts_ms32`: 발신 시점 UTC epoch ms 하위 32비트 (`core/include/server/core/protocol/packet.hpp`)
- Body: `length` 바이트.

### Flags 비트 정의(초안)
- bit0: COMPRESSED(LZ4) (core/include/server/core/protocol/protocol_flags.hpp:9)
- bit1: ENCRYPTED(TLS 외 경량 암호화 사용 시) (core/include/server/core/protocol/protocol_flags.hpp:10)
- bit2~15: 예약(SEQ/TS는 고정 헤더 포함이므로 플래그 불요) (core/include/server/core/protocol/protocol_flags.hpp:11)

## 메시지 ID(opcodes)

- 소스 오브 트루스(JSON)
  - system(core): `core/protocol/system_opcodes.json`
  - game(server): `server/protocol/game_opcodes.json`
- 생성 헤더(C++)
  - system(core): `core/include/server/core/protocol/system_opcodes.hpp`
  - game(shared): `core/include/server/protocol/game_opcodes.hpp`
- 생성기
  - C++ 헤더 생성: `tools/gen_opcodes.py` (CMake custom command로 빌드 시 자동 실행; Python3 필요)
  - 문서/검증: `tools/gen_opcode_docs.py` → `docs/protocol/opcodes.md` (system/game 전체 16-bit 공간 중복 검증 포함)

Sapphire처럼 도메인별로 보기 좋게 관리하기 위해 JSON에 `groups[]`(범위)와 opcode별 `group`/`dir(c2s|s2c)`를 둔다.
실제 목록은 `docs/protocol/opcodes.md`를 참고한다.

## 길이 제한/보안
- `length` 상한: 기본 32KB(설정 가능). 초과 시 세션 종료. (core/src/net/session.cpp:147)
- 압축: `flags`에 LZ4 표시, 임계치(예: 1KB) 초과 시 적용.
- 암호화: 모든 상용 배포는 TLS(Load Balancer ↔ Gateway, Gateway ↔ Server) 종단을 필수로 사용한다. 개발·테스트 환경에서만 ChaCha20-Poly1305 기반 경량 암호 혹은 XOR 샘플을 허용하며, 이때도 `HELLO.capabilities`에서 `CAP_PLAINTEXT_ACCEPTED`를 교환해야 한다.

## 시퀀스/재전송
- `seq`와 `utc_ts_ms32`는 항상 고정 헤더에 포함된다. (`core/include/server/core/protocol/packet.hpp`)
- 현재는 재전송/ACK 등의 신뢰 레이어를 구현하지 않으며, `seq`는 클라이언트/서버 측 추적/디버깅/순서 확인 용도로만 사용한다. (core/src/net/session.cpp:48)
- 향후 기본 OFF 상태로 도입할 RUDP 설계/단계 계획은 `docs/protocol/rudp.md`를 따른다.

## 예시 인코딩
```
[00 0A] [01 00] [00 00] [00 00 00 2A] [00 00 01 89] | payload(10B)
 length  msg_id   flags     seq(uint32)      utc_ts_ms32
```

## 호환성 전략
- msg_id 예약 구간을 모듈별로 분배(예: 0x0000~0x0FFF: 코어, 0x1000~: 게임).
- 스키마 변경 시: payload 내 `version` 필드를 추가하거나, 기존 메시지를 유지한 채 새로운 `msg_id`를 정의해 하위 호환을 보장한다. 서버는 항상 낮은 버전 클라이언트의 필드를 optional 로 처리하고, `HELLO.capabilities`에서 협상 가능한 최대 버전을 노출한다.

## 브로드캐스트 Envelope 규칙
- Redis Pub/Sub 브로드캐스트는 `gw=<gateway_id>` + `\n` + Protobuf `ChatBroadcast` 바이트를 그대로 publish한다. 첫 줄의 gateway ID는 self-echo 필터에 사용되며, 동일 ID가 다시 수신되면 즉시 드롭한다.
- 필요 시 `room=<room_name>`과 `ts_ms=<epoch_ms>` 헤더를 추가한다. 헤더와 payload는 빈 줄로 구분하며, 헤더가 없으면 `gw=` 줄 뒤에 바로 Protobuf 바이트가 이어진다.
- `sender_sid`는 시스템/공지 메시지일 때 `0`으로 채우고, 일반 메시지는 Session ID를 넣어 클라이언트가 self-echo 처리를 할 수 있게 한다.
- `ts_ms`는 UTC epoch milliseconds를 사용하며, gateway가 수신한 시각이 아닌 서버에서 메시지를 생성한 시각을 기록한다.
- `trace_id`, `correlation_id`, `tenant`, `user_id` 등 메타데이터는 Protobuf payload 안에서 유지하며 헤더는 최소 정보만 담는다.

---

## 내부 프로토콜(서비스 간)

### RPC(gRPC)
- 권장: Protobuf IDL. 예시 서비스:
  - Auth: `Login`, `ValidateToken`, `RefreshToken`.
  - Chat: `JoinRoom`, `LeaveRoom`, `SendMessage`, `ListRooms`.
  - Presence: `SetOnline`, `SetOffline`, `GetPresence`.


### 이벤트(Event Bus)
- Topic 형식은 `<service>.events.<noun>`을 사용한다. 예: `chat.events.message`, `presence.events.online`.
- 메시지 payload 는 `{ room_id, sender_id, text, ts_ms, correlation_id }` 구조의 JSON 또는 Protobuf map을 사용하며, 서버는 `ts_ms`(UTC ms)를 채운다.
- `idempotency_key`(예: message_id + shard)를 포함해 중복 소비를 방지하고, 소비자는 키 기반으로 ACK 한다.
- Chat 브로드캐스트 구독자는 Pub/Sub Envelope 규칙을 그대로 재사용하며 self-echo 필터를 반드시 적용한다.

### 보안
- 외부 링크(클라이언트↔Gateway, Gateway↔Load Balancer)는 TLS 1.3 이상을 강제한다. 내부 RPC도 mTLS를 권장하며, 자체 CA를 사용할 수 없으면 관리형 인증서를 배포한다.
- 인증 토큰은 JWT 또는 opaque token 모두 지원하지만, 평문 비밀번호는 네트워크에 노출하지 않는다. `MSG_LOGIN_REQ`에서는 pre-hash+salt를 사용한다.
- Rate limit 및 lockout 정책을 Redis 기반으로 유지하고, `MSG_ERR(RATE_LIMITED)` 응답을 통해 클라이언트에 알린다.
- RBAC는 `scope` 필드로 표현하며, Gateway는 scope에 따라 허용된 opcode만 라우팅한다.

---

## 클라이언트 핸드셰이크 (`core/src/net/session.cpp`)
- 흐름: Client가 TCP 연결을 맺으면 서버가 `MSG_HELLO`를 먼저 보내고, 클라이언트는 capabilities를 확인한 뒤 `MSG_LOGIN_REQ` 또는 `MSG_STATE_SNAPSHOT_REQ`를 전송한다.
- `MSG_HELLO` payload (v1.1, 12 bytes):
  - `uint16 proto_major`, `uint16 proto_minor`: 서버가 지원하는 프로토콜 버전(`core/include/server/core/protocol/version.hpp`).
  - `uint16 capabilities`: 비트 플래그. `CAP_COMPRESS_SUPP`, `CAP_SENDER_SID`, `CAP_ROOM_DELTA` 등을 정의한다.
  - `uint16 heartbeat_interval_x10ms`: 서버가 요구하는 heartbeat 주기를 10ms 단위로 표현.
  - `uint32 epoch_high32`: 서버 시각(ms)의 상위 32비트로, 클라이언트가 `utc_ts_ms32` wrap을 보정하는 데 사용한다.
- `MSG_LOGIN_REQ` payload는 기존 `user/token` 뒤에 선택적으로 `uint16 client_proto_major`, `uint16 client_proto_minor`를 붙일 수 있다.
  - 버전 판정 규칙: `major`는 반드시 일치, `minor`는 서버 버전 이하만 허용.
  - 불일치 시 서버는 `MSG_ERR(UNSUPPORTED_VERSION)`를 반환한다.
  - 버전 필드가 없는 기존 클라이언트는 서버 기본 버전으로 간주해 허용한다.
- 클라이언트는 HELLO 수신 후 `MSG_PING`/`MSG_LOGIN_REQ`를 보내기 전까지 heartbeat 타이머를 설정해야 한다.

### MSG_ERR 코드 예약 (core/include/server/core/protocol/protocol_errors.hpp:11)
- 0x0007: INVALID_PAYLOAD
- 0x0009: UNSUPPORTED_VERSION
- 0x0100: NAME_TAKEN(닉네임 중복)
- 필요 시 확장: MALFORMED_FRAME, FORBIDDEN, RATE_LIMITED, INTERNAL_ERROR 등은 도입 시 코드 예약 후 사용

## 문자열/인코딩 규칙(중요) (`core/include/server/core/protocol/packet.hpp`)
- 모든 문자열은 UTF-8로 인코딩한다(서버/클라 모두). (`core/include/server/core/protocol/packet.hpp`)
- 문자열 필드는 길이-접두(prefix) 방식 사용을 권장: `uint16 len` + `len` 바이트(UTF-8). 널 종료는 사용하지 않는다.

## 메시지 payload 스키마(발췌)
- `MSG_LOGIN_REQ`: `u16 len_user | user(UTF-8)` + `u16 len_secret | secret(UTF-8)` 형태로 전송하며 secret은 pre-hash된 값이다.
- `MSG_LOGIN_RES`: `{ user_id(uuid), session_id(uuid), effective_user, expires_at_ms }` JSON을 포함한다.
- `MSG_CHAT_SEND`: `{ room_id(uuid), content(lp_utf8), reply_to(optional u64) }`.
- `MSG_CHAT_BROADCAST`: `{ room_id, message_id(u64), sender_id(uuid), sender_sid(u32), created_at_ms(i64), content(lp_utf8) }`.
- `MSG_STATE_SNAPSHOT`: `{ room_id(uuid), wm(u64), count(u16), messages[] }` 구조를 따르며 메시지는 id 오름차순이다.
- `sender_sid`는 `HELLO.capabilities`에 `CAP_SENDER_SID`가 포함된 경우에만 채우고, 이전 클라이언트와의 호환을 위해 optional로 남긴다.
- 게이트웨이는 브로드캐스트 payload마다 `ts_ms`를 UTC epoch milliseconds로 채운다.

## 인증 메시지
- MSG_REGISTER_REQ/RES: { user, password_sha256, email(optional) } → { user_id, pending_verification }. 비밀번호는 Argon2id로 저장한다.
- MSG_LOGIN_REQ/RES: { user, secret_prehashed } → { session_id(uuid), token, expires_at_ms }. Redis rate limit을 적용한다.
- MSG_REFRESH_REQ/RES: { refresh_token } → { session_id, token, expires_at_ms }. Refresh 토큰은 1회용이다.
- MSG_LOGOUT_REQ: { session_id }를 전송하면 서버와 SessionDirectory에서 즉시 제거한다.
- 추가 정책: TLS 강제, 평문 pw 금지, rate limit, lockout(5회 실패 시 5분 차단).

