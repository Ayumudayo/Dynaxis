# 프로토콜 설계

이 문서는 Dynaxis wire/runtime 프로토콜의 대표 진입 문서다. 세부 프로토콜 계약을 한 파일에 모두 몰아넣지 않고, 실제 저장소에서 유지하는 세 갈래 문서로 나누어 관리한다. 이렇게 나누는 이유는 전송 계층 문제, opcode 목록 문제, 스냅샷/상태 문제의 변경 속도와 검증 방식이 서로 다르기 때문이다. 모든 내용을 한 문서에 섞어 두면 변경 한 번마다 전체 문서를 다시 읽어야 하고, 어느 부분이 단일 기준 문서인지도 흐려진다.

현재 상태에서 세부 계약은 아래 문서를 기준으로 본다.

- `docs/protocol/opcodes.md`
  - 생성된 opcode 목록 문서
- `docs/protocol/rudp.md`
  - UDP/RUDP 전송 동작
- `docs/protocol/snapshot.md`
  - room snapshot, recent-history cache, join-to-fanout 흐름

## 핵심 원칙

- 외부 프로토콜과 내부 프로토콜을 분리한다.
  - 클라이언트와 gateway 사이의 계약과, 서비스 간 내부 동작은 같은 속도로 진화하지 않기 때문이다.
- 패킷 헤더와 프레이밍의 단일 기준은 `core/include/server/core/protocol/packet.hpp`다.
  - 프레이밍 규칙이 여러 파일에 흩어지면 서로 다른 길이 계산과 flag 해석이 생기기 쉽다.
- 모든 정수는 네트워크 바이트 순서(network byte order, big-endian)를 사용한다.
  - 플랫폼별 기본 엔디언 차이로 인한 해석 오류를 막기 위해서다.
- 문자열은 길이 접두(length-prefixed) UTF-8을 사용한다.
  - null 종료 문자열에 의존하면 길이 계산, 중간 null, 바이너리 안전성에서 문제가 생긴다.
- 기준 식별자는 이름이 아니라 UUID(`user_id`, `room_id`, `session_id`)다.
  - 사람이 읽기 좋은 이름은 바뀔 수 있지만, 런타임 식별성(runtime identity)은 바뀌면 안 되기 때문이다.

## 기본 패킷 형식

- 헤더 14바이트:
  - `uint16 length`
  - `uint16 msg_id`
  - `uint16 flags`
  - `uint32 seq`
  - `uint32 utc_ts_ms32`
- 본문:
  - `length` bytes

flag 표준 정의는 `core/include/server/core/protocol/protocol_flags.hpp`를 따른다.

헤더를 작고 고정된 형태로 유지하는 이유는 파서, 로거, 테스트 도구, loadgen이 모두 같은 전제를 공유해야 하기 때문이다. 헤더가 상황별로 달라지기 시작하면 디코더는 복잡해지고, 잘못된 입력을 방어하는 비용도 커진다.

## Opcode 단일 기준

- system(core): `core/protocol/system_opcodes.json`
- game(server): `server/protocol/game_opcodes.json`
- 생성 헤더:
  - `core/include/server/core/protocol/system_opcodes.hpp`
  - `core/include/server/protocol/game_opcodes.hpp`
- 생성 문서:
  - `docs/protocol/opcodes.md`

Dynaxis가 opcode를 JSON 원본에서 생성하는 이유는 식별자, 설명, 중복 검사를 한곳에서 관리하기 위해서다. 헤더와 문서를 각각 수동으로 관리하면 다음 문제가 바로 생긴다.

- 같은 `msg_id`가 두 곳에서 다른 의미로 쓰일 수 있다.
- 헤더는 바뀌었는데 문서는 예전 상태로 남을 수 있다.
- 테스트와 CI가 "무엇이 정답인지" 판단할 단일 기준을 잃는다.

즉, 이 계층의 목표는 편의가 아니라 문서와 코드의 어긋남(drift) 방지다.

## 현재 동작 분리

### 전송 계층

- 기본 신뢰 경로는 TCP다.
- direct UDP/RUDP 동작과 rollout/fallback 계약은 `docs/protocol/rudp.md`를 따른다.
- 현재 gameplay-frequency 검증 표면은 direct `MSG_PING`, `MSG_FPS_INPUT`, `MSG_FPS_STATE_DELTA`까지 확장되어 있다.

전송 문서를 따로 두는 이유는 전송 계층이 다른 계층보다 실패 형태가 훨씬 많기 때문이다. handshake, retransmit, fallback, allowlist, canary, NAT/edge 문제는 opcode 목록만 봐서는 이해할 수 없다. 그래서 전송 계층은 별도 계약으로 관리한다.

### 상태와 스냅샷

- room state snapshot과 join 이후 fanout 연결 규칙은 `docs/protocol/snapshot.md`를 따른다.
- snapshot payload, watermark, duplicate filtering, recent-history cache fallback의 단일 기준도 그 문서다.

상태 문서를 전송 문서와 분리한 이유는 패킷을 어떻게 보내느냐와 "어떤 상태를 어느 시점에 보내느냐"가 다른 종류의 문제이기 때문이다. 이 둘을 섞으면 join 시점의 정합성 문제를 전송 버그처럼 보거나, 반대로 전송 지연을 snapshot 설계 문제로 오해하기 쉽다.

## 보안과 호환성

- 상용 배포는 TLS를 기본으로 한다.
- 프로토콜 확장은 기존 메시지를 유지한 채 신규 `msg_id`를 추가하거나 payload version field로 처리한다.
- 낮은 버전 클라이언트와의 호환은 선택 필드(optional field)와 capability negotiation으로 관리한다.

이 원칙을 지키는 이유는, 이미 배포된 클라이언트를 한 번에 모두 교체할 수 없기 때문이다. 기존 메시지를 조용히 바꿔 버리면 가장 먼저 깨지는 것은 구버전 클라이언트지만, 실제 원인을 찾는 쪽은 운영자다. 그래서 Dynaxis는 "기존 의미 유지 + 새 항목 추가"를 기본 전략으로 둔다.

## 관련 문서

- `docs/core-design.md`
- `docs/configuration.md`
- `docs/ops/gateway-and-lb.md`
