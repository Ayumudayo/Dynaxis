# RUDP 설계 초안 (Phase 0, 기본 OFF)

상태: `staged` (구현은 존재하지만 런타임 기본 경로는 비활성)

진행 상태 메모:

- Phase 1/2 범위로 `core/include/server/core/net/rudp/*`, `core/src/net/rudp/*`에 엔진/ACK/재전송 기본 구현이 추가되었다.
- Phase 3 범위로 `gateway/src/gateway_app.cpp`에 RUDP adapter 분기(기본 OFF + canary + opcode allowlist + 세션 fallback)가 추가되었다.
- 기본 경로는 여전히 OFF이며, `GATEWAY_RUDP_ENABLE=0` 또는 `GATEWAY_RUDP_CANARY_PERCENT=0`/allowlist 비어 있음 상태에서는 기존 경로를 사용한다.

## 1. 목표

- 기존 TCP 기본 경로와 기존 UDP bind control plane을 유지한다.
- 애플리케이션 프레임(`PacketHeader` 14 bytes) 규약을 변경하지 않는다.
- 향후 활성화를 위해 `core`에 재사용 가능한 RUDP 엔진 경계를 먼저 고정한다.

## 2. 비목표

- 즉시 프로덕션 활성화.
- 기존 TCP 세션/핸들러를 대규모 리라이트.
- 초기 단계에서 MTU 동적 탐색/복잡한 혼잡 제어를 도입.

## 3. 선행 조건과 활성화 게이트

선행 조건:

1. TCP 인증 세션이 정상 수립되어야 한다.
2. 기존 UDP bind(`MSG_UDP_BIND_REQ/RES`)가 성공해야 한다.
3. bind된 endpoint에서만 RUDP handshake를 허용한다.

활성화 게이트(기본 OFF):

- 빌드: `KNIGHTS_ENABLE_CORE_RUDP=OFF`
- 런타임: `GATEWAY_RUDP_ENABLE=0`, `GATEWAY_RUDP_CANARY_PERCENT=0`

## 4. 전송 모델

- Inner frame: 기존 앱 프레임(`PacketHeader` + payload)을 그대로 사용한다.
- Outer frame: UDP datagram 위에 RUDP 제어 헤더를 추가한다.
- dispatcher 정책(`TransportMask`, `DeliveryClass`)은 기존 경로를 재사용한다.

### 4.1 RUDP outer header (v1 draft)

모든 필드는 network byte order(big-endian)를 사용한다.

| 필드 | 타입 | 설명 |
| --- | --- | --- |
| `magic` | `u16` | RUDP 식별자(`0x5255`, "RU") |
| `version` | `u8` | 프로토콜 버전 |
| `type` | `u8` | `HELLO`, `HELLO_ACK`, `DATA`, `PING`, `CLOSE` |
| `conn_id` | `u32` | 세션 범위 연결 식별자 |
| `pkt_num` | `u32` | 송신 패킷 번호 |
| `ack_largest` | `u32` | 가장 큰 연속 ACK 번호 |
| `ack_mask` | `u64` | 선택 ACK 비트마스크(최근 64개) |
| `ack_delay_ms` | `u16` | ACK 지연(ms) |
| `channel` | `u8` | `DeliveryClass` 매핑 채널 |
| `flags` | `u8` | `ACK_ONLY`, `RETRANSMIT` 등 |
| `timestamp_ms` | `u32` | RTT 추정을 위한 송신 시각 |
| `payload_len` | `u16` | inner frame 길이 |

payload는 기존 앱 프레임 바이트열을 그대로 담는다.

## 5. 핸드셰이크 상태기계

상태: `Idle -> HelloSent/HelloRecv -> Established -> Draining/Closed`

1. Client가 bind 성공 후 `HELLO`를 전송한다.
2. Server가 `HELLO_ACK`로 버전/기능/MTU를 확정한다.
3. 양측이 `Established`로 전환되기 전까지 앱 데이터는 TCP만 사용한다.
4. handshake timeout/검증 실패 시 해당 세션은 TCP fallback으로 고정한다.

검증 항목:

- bind된 session endpoint 일치 여부
- nonce/token/session-cookie 일치 여부
- version/capability 협상 가능 여부

## 6. 신뢰성 규칙 (초기)

- ACK: `ack_largest + ack_mask(64)`
- 재전송: RTO 기반(예: `rto = srtt + 4*rttvar`, min/max clamp)
- in-flight 제한: 패킷/바이트 상한을 모두 적용
- delayed ACK: 기본 5~10ms, out-of-order 감지 시 즉시 ACK
- keepalive: 유휴 구간 `PING`으로 NAT/RTT 상태 유지

기본 파라미터(초안):

- `mtu_payload_bytes=1200`
- `max_inflight_packets=256`
- `max_inflight_bytes=262144`
- `rto_min_ms=50`, `rto_max_ms=2000`

## 7. fallback/rollback 정책

세션 단위 fallback 조건:

- handshake timeout/검증 실패
- ACK 진행 정체(`ack_stall_ms` 초과)
- 재전송 비율 급증(운영 임계치 초과)

롤백 순서:

1. `GATEWAY_RUDP_CANARY_PERCENT=0`
2. `GATEWAY_RUDP_ENABLE=0`
3. TCP KPI 정상 복귀 확인(연결 성공률, 지연, 오류율)

## 8. 계획 메트릭/알람

계획 메트릭:

- `core_runtime_rudp_handshake_total{result}`
- `core_runtime_rudp_retransmit_total`
- `core_runtime_rudp_inflight_packets`
- `core_runtime_rudp_rtt_ms_*`
- `core_runtime_rudp_fallback_total{reason}`

계획 알람:

- `RudpHandshakeFailureSpike`
- `RudpRetransmitRatioHigh`
- `RudpFallbackSpike`

## 9. 테스트 매트릭스

- 단위: ACK/window wrap, retransmit timer, reorder/dup 처리
- 통합: bind->handshake->mixed transport 분기
- 회귀: RUDP OFF에서 기존 TCP 경로 무변경
- 운영: canary 0->N% 확대/축소와 10분 내 rollback 리허설

## 10. 참조

- `core/include/server/core/protocol/packet.hpp`
- `core/include/server/core/protocol/opcode_policy.hpp`
- `server/include/server/protocol/game_opcodes.hpp`
- `gateway/src/gateway_app.cpp`
- `docs/ops/udp-rollout-rollback.md`

### 10.1 외부 설계 참고 (구현 시 재검토)

- RFC 6298 (RTO 계산 표준): https://www.rfc-editor.org/rfc/rfc6298
- RFC 6675 (SACK 기반 손실 복구): https://datatracker.ietf.org/doc/rfc6675/
- KCP (경량 RUDP 구현 참고): https://github.com/skywind3000/kcp
- ENet (게임용 신뢰 UDP 패턴): http://enet.bespin.org/Features.html
- O3DE AzNetworking UDP reliability 구성 예시:
  - https://github.com/o3de/o3de/blob/development/Code/Framework/AzNetworking/AzNetworking/UdpTransport/UdpPacketIdWindow.h
  - https://github.com/o3de/o3de/blob/development/Code/Framework/AzNetworking/AzNetworking/UdpTransport/UdpReliableQueue.h
