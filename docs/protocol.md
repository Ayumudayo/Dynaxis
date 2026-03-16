# 프로토콜 설계

이 문서는 Dynaxis wire/runtime protocol의 entrypoint다.
현재 상태에서 세부 계약은 아래 세 문서로 나뉜다.

- `docs/protocol/opcodes.md` - generated opcode catalog
- `docs/protocol/rudp.md` - UDP/RUDP transport behavior
- `docs/protocol/snapshot.md` - room snapshot, recent-history cache, join-to-fanout flow

## 핵심 원칙

- 외부(클라이언트↔Gateway)와 내부(서비스 간) 프로토콜을 분리한다.
- 패킷 헤더/프레이밍의 단일 소스는 `core/include/server/core/protocol/packet.hpp`다.
- 모든 정수는 network byte order(big-endian)를 사용한다.
- 문자열은 length-prefixed UTF-8을 사용한다.
- canonical 식별자는 이름이 아니라 UUID(`user_id`, `room_id`, `session_id`)다.

## 기본 패킷 형식

- Header 14 bytes:
  - `uint16 length`
  - `uint16 msg_id`
  - `uint16 flags`
  - `uint32 seq`
  - `uint32 utc_ts_ms32`
- Body: `length` bytes

Flags 표준 정의는 `core/include/server/core/protocol/protocol_flags.hpp`를 따른다.

## Opcode Source Of Truth

- system(core): `core/protocol/system_opcodes.json`
- game(server): `server/protocol/game_opcodes.json`
- generated headers:
  - `core/include/server/core/protocol/system_opcodes.hpp`
  - `core/include/server/protocol/game_opcodes.hpp`
- generated docs:
  - `docs/protocol/opcodes.md`

## Current Behavioral Splits

### Transport

- 기본 신뢰 경로는 TCP다.
- direct UDP/RUDP behavior와 rollout/fallback contract는 `docs/protocol/rudp.md`를 따른다.
- 현재 gameplay-frequency proof surface는 direct `MSG_PING`, `MSG_FPS_INPUT`, `MSG_FPS_STATE_DELTA`까지 확장되어 있다.

### State / Snapshot

- room state snapshot과 join 이후 fanout 연결 규칙은 `docs/protocol/snapshot.md`를 따른다.
- snapshot payload, watermark, duplicate filtering, recent-history cache fallback은 그 문서가 source of truth다.

## Security / Compatibility

- 상용 배포는 TLS를 기본으로 한다.
- 프로토콜 확장은 기존 메시지 유지 + 신규 `msg_id` 추가 또는 payload version field로 처리한다.
- 낮은 버전 클라이언트와의 호환은 optional field와 capability negotiation으로 관리한다.

## Related Docs

- `docs/core-design.md`
- `docs/configuration.md`
- `docs/ops/gateway-and-lb.md`
