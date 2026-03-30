# RUDP 설계 초안 (Phase 0, 런타임 기본 비활성)

상태: `staged`
설명: 구현은 존재하지만 런타임 기본 경로는 아직 비활성이다.

이 문서는 RUDP를 "곧바로 기본 경로로 바꾸기 위한 선언"이 아니라, 나중에 안전하게 활성화할 수 있도록 경계를 먼저 고정해 두기 위한 설계 문서다. 이런 문서가 필요한 이유는 UDP 계열 전송은 한 번 열리면 성능 실험은 빨라지지만, 반대로 실패 모드도 TCP보다 훨씬 많아지기 때문이다. 따라서 활성화 전에 handshake, 재전송, fallback, 관측 지점을 미리 문서로 고정해 두는 편이 유지보수에 유리하다.

진행 상태 메모:

- Phase 1/2 범위로 `core/include/server/core/net/rudp/*`, `core/src/net/rudp/*`에 엔진과 ACK, 재전송 기본 구현이 추가되었다.
- Phase 3 범위로 `gateway/src/gateway_app.cpp`에 RUDP adapter 분기(기본 비활성 + canary + opcode allowlist + 세션 fallback)가 추가되었다.
- Phase 4 범위로 impairment, flow-control, fallback 단위 테스트(`tests/core/test_rudp_*.cpp`)를 확장했고, CI의 단일 `Run Windows Unit Tests (ctest)` 단계에서 함께 검증한다.
- Phase 5 범위로 `docker/observability/prometheus/alerts.yml`, `alerts.tests.yml`에 RUDP rollout 경보를 추가하고 운영 문서(runbook, fallback, observability)를 동기화했다.
- 기본 경로는 여전히 비활성이며, `GATEWAY_RUDP_ENABLE=0` 또는 `GATEWAY_RUDP_CANARY_PERCENT=0`, allowlist 비어 있음 상태에서는 기존 경로를 사용한다.

## 1. 목표

- 기존 TCP 기본 경로와 기존 UDP bind 제어면(control plane)을 유지한다.
- 애플리케이션 프레임(`PacketHeader` 14 bytes) 규약을 변경하지 않는다.
- 향후 활성화를 위해 `core`에 재사용 가능한 RUDP 엔진 경계를 먼저 고정한다.

이 목표가 중요한 이유는 전송 방식을 바꾼다고 해서 애플리케이션 프레임 계약까지 함께 흔들 필요는 없기 때문이다. 바깥 전송 껍질만 바꾸고 내부 프레임을 유지하면, dispatcher, 로거, opcode 정책, 테스트 도구가 같은 전제를 계속 공유할 수 있다.

## 2. 비목표

- 즉시 프로덕션 기본 경로로 활성화
- 기존 TCP 세션과 핸들러의 대규모 재작성
- 초기 단계에서 MTU 동적 탐색이나 복잡한 혼잡 제어 도입

비목표를 명확히 적는 이유는 RUDP 도입 논의가 쉽게 "그럼 TCP를 다 바꾸자"로 번지기 때문이다. 그렇게 되면 위험한 변경을 한 번에 너무 많이 묶게 되고, 문제가 생겼을 때 어느 계층이 원인인지 구분하기 어려워진다.

## 3. 선행 조건과 활성화 게이트

선행 조건:

1. TCP 인증 세션이 정상 수립되어야 한다.
2. 기존 UDP bind(`MSG_UDP_BIND_REQ/RES`)가 성공해야 한다.
3. bind된 endpoint에서만 RUDP handshake를 허용한다.

활성화 게이트(기본 비활성):

- 런타임: `GATEWAY_RUDP_ENABLE=0`, `GATEWAY_RUDP_CANARY_PERCENT=0`

이 게이트가 필요한 이유는 RUDP를 "켜면 곧바로 모두 사용"하는 방식으로 운영하지 않기 위해서다. 전송 실험은 세션 단위, 비율 단위, opcode 단위로 좁혀서 여는 편이 rollback과 원인 분석이 훨씬 쉽다.

## 4. 전송 모델

- inner frame: 기존 앱 프레임(`PacketHeader` + payload)을 그대로 사용한다.
- outer frame: UDP datagram 위에 RUDP 제어 헤더를 추가한다.
- dispatcher 정책(`TransportMask`, `DeliveryClass`)은 기존 경로를 재사용한다.

inner와 outer를 분리하는 이유는 애플리케이션 의미와 전송 제어 의미를 섞지 않기 위해서다. 둘을 한 헤더에 같이 집어넣기 시작하면, 전송 계층 실험이 곧바로 앱 계층 계약 변경으로 번진다.

### 4.1 RUDP 바깥 헤더 (v1 초안)

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

## 5. 핸드셰이크 상태 기계

상태: `Idle -> HelloSent/HelloRecv -> Established -> Draining/Closed`

1. client가 bind 성공 뒤 `HELLO`를 전송한다.
2. server가 `HELLO_ACK`로 버전, 기능, MTU를 확정한다.
3. 양측이 `Established`로 전환되기 전까지 앱 데이터는 TCP만 사용한다.
4. handshake timeout이나 검증 실패가 나면 해당 세션은 TCP fallback으로 고정한다.

검증 항목:

- bind된 session endpoint 일치 여부
- nonce, token, session-cookie 일치 여부
- version과 capability 협상 가능 여부

여기서 가장 중요한 원칙은 "성공이 확정되기 전까지는 TCP를 계속 신뢰 경로로 둔다"는 점이다. 그렇지 않으면 반쯤 열린 RUDP 세션 때문에 로그인 직후 데이터 손실이나 유령 연결 문제가 생길 수 있다.

## 6. 신뢰성 규칙 (초기)

- ACK: `ack_largest + ack_mask(64)`
- 재전송: RTO 기반(예: `rto = srtt + 4*rttvar`, min/max clamp)
- in-flight 제한: 패킷 상한과 바이트 상한을 모두 적용
- delayed ACK: 기본 5~10ms, out-of-order 감지 시 즉시 ACK
- keepalive: 유휴 구간 `PING`으로 NAT와 RTT 상태 유지

기본 파라미터(초안):

- `mtu_payload_bytes=1200`
- `max_inflight_packets=256`
- `max_inflight_bytes=262144`
- `rto_min_ms=50`, `rto_max_ms=2000`

이 규칙을 초기에 단순하게 두는 이유는, 복잡한 혼잡 제어보다 먼저 "실패했을 때 TCP로 안전하게 돌아갈 수 있는가"를 증명하는 편이 더 중요하기 때문이다.

## 7. fallback과 rollback 정책

세션 단위 fallback 조건:

- handshake timeout 또는 검증 실패
- ACK 진행 정체(`ack_stall_ms` 초과)
- 재전송 비율 급증(운영 임계치 초과)

rollback 순서:

1. `GATEWAY_RUDP_CANARY_PERCENT=0`
2. `GATEWAY_RUDP_ENABLE=0`
3. TCP KPI 정상 복귀 확인(연결 성공률, 지연, 오류율)

rollback 순서를 따로 적는 이유는 RUDP는 "부분 실패"가 흔한 계층이기 때문이다. 세션별 fallback과 전체 비활성화를 분리해 두면, 문제를 좁혀 가며 되돌릴 수 있다.

## 8. 계획 메트릭과 알람

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

전송 실험에서 가장 위험한 것은 "느리게 망가지는 상태"다. 그래서 단순 성공 여부뿐 아니라 재전송, RTT, fallback 비율을 함께 봐야 한다.

## 9. 테스트 매트릭스

- 단위: ACK, window wrap, retransmit timer, reorder/dup 처리
- 통합: bind -> handshake -> mixed transport 분기
- 회귀: RUDP 비활성 상태에서 기존 TCP 경로 무변경
- 운영: canary 0 -> N% 확대/축소와 10분 내 rollback 리허설

테스트를 이 층으로 나누는 이유는 실패 형태도 층마다 다르기 때문이다. 단위 테스트는 패킷 수준 오류를, 통합 테스트는 handshake 연결을, 운영 검증은 rollout과 rollback 현실성을 본다.

## 10. 참조

- `core/include/server/core/protocol/packet.hpp`
- `core/include/server/core/protocol/opcode_policy.hpp`
- `core/include/server/protocol/game_opcodes.hpp`
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
