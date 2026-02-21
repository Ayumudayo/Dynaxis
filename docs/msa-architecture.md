# 마이크로서비스(MSA) 아키텍처 상세 설계

## 서비스 분해
- Gateway Service: TCP 세션/프레이밍/기본 정책, 외부 클라이언트 유일 진입점.
- Auth Service: 로그인/토큰 발급·검증/갱신.
- Chat Service: 채널/룸/브로드캐스트/귓속말/필터.
- Match Service: 매치메이킹 큐/룰/매치 생성 이벤트.
- World Service: 월드/존 상태, 엔티티 관리, 브로드캐스트 최적화.
- Presence Service: 사용자 온라인/오프라인, 라우팅 키, 상태 이벤트.

## 통신 모델
- Client ↔ Gateway: TCP(바이너리 프로토콜, `protocol.md`).
- Gateway ↔ Services: gRPC(HTTP/2, Protobuf). 성능 민감 경로는 고정 연결/커넥션 풀.
  - Gateway는 사용자 세션 컨텍스트를 메타데이터로 전파: `user_id`, `session_id`, `authz_scope`, `trace_id`.
  - 외부 프로토콜 문자열은 UTF-8, 길이-접두 방식으로 일관 처리.
- Services ↔ Services: 이벤트 버스(NATS/Kafka/Redis Streams). 주제(topic) 예:
  - `auth.events.login`, `presence.events.state`, `chat.events.message`, `match.events.created`.
- 트레이싱/상관관계: `trace_id`, `span_id`, `correlation_id`를 헤더/메시지에 전파.

## 데이터/일관성
- 각 서비스는 자체 저장소(Polyglot Persistence)와 스키마를 소유.
- 일관성: 핵심 경로(auth)는 강한 일관성, 나머지는 이벤트 기반 최종 일관성.
- 멱등성: `idempotency_key` 도입(재전송·중복 방지).

## 확장/고가용성
- 서비스 단위 수평 확장(HPA/오토스케일). Gateway는 L4/L7 로드밸런서 뒤에 다중 인스턴스.
- 상태: 세션은 Gateway에 고정되지만 라우팅 키/Presence로 다른 서비스가 대상 사용자 식별.
- 장애 내성: Circuit Breaker/Retry with backoff/Bulkhead. 버스 장애 시 로컬 큐에 임시 버퍼링.

## 보안
- 외부: TLS, JWT.
- 내부: mTLS, 서비스 계정/권한, 보안 헤더(알려진 발행자/청중 검증).

## 게이트웨이 라우팅 규칙(상세)
- 상태: `PreAuth` ↔ `PostAuth`.
- PreAuth 허용: `HELLO`, `PING`, `LOGIN_REQ`.
- 토큰 검증: `ValidateToken` gRPC(Auth)에 위임. 성공 시 세션에 `user_id`, `scopes` 바인딩.
- PostAuth 라우팅: `msg_id -> (service, rpc)` 테이블. 예: `CHAT_SEND -> Chat.SendMessage`.
- 권한: 라우팅 전 `scopes`로 최소 권한 확인, 실패 시 `ERR(FORBIDDEN)`.
- QoS/Rate limit: IP/사용자/메시지별 토큰 버킷. 초과 시 `ERR(RATE_LIMITED)`.

## 메시지 버스 선택(결정/근거 제안)
- MVP 기본: NATS(단순 운영, 낮은 지연, at-most-once/at-least-once 선택 용이, JetStream로 내구성 옵션).
- 대체: Kafka(높은 내구/스루풋, 운영 복잡도↑), Redis Streams(간편, 단일 인스턴스 한계).
- 설계는 추상화 계층으로 교체 가능하게 유지.

## 배포/구성/발견
- 초기: 정적 설정 파일 기반. 이후: 서비스 디스커버리(Consul/etcd)로 전환.
- 설정은 구성 서비스 또는 핫 리로드로 배포.

## 관측성
- 메트릭: 요청율/오류율/지연, 큐 길이, 세션 수, 버스 지연.
- 로그: 구조화 로그(json), trace id 포함.
- 트레이싱: OpenTelemetry, 샘플링 정책.

## 워크플로우(예: 로그인 → 채널 입장 → 채팅)
1) Client → Gateway: `MSG_LOGIN_REQ`
2) Gateway → Auth: `Login` gRPC
3) Auth → Gateway: 토큰/프로필
4) Gateway → Presence: 온라인 등록(이벤트)
5) Client → Gateway: `MSG_JOIN_ROOM`
6) Gateway → Chat: `JoinRoom` gRPC
7) Client → Gateway: `MSG_CHAT_SEND`
8) Gateway → Chat: `SendMessage` gRPC → Chat → BUS: `chat.events.message`
9) Gateway: 룸 구독자 세션에 브로드캐스트

## 서비스 경계/계약(추가)
- Auth: Register/Login/Refresh/Logout, opaque 세션 토큰(내부는 JWT 옵션)
- Presence: GetPresence 배치, 룸/유저 카디널리티 주의 및 폴백
- Gateway: heartbeat 집계→Redis SETEX, Pub/Sub 브릿지(envelope: gateway_id, origin)
- 이벤트 토픽(초안): auth.events.login, presence.events.state, chat.events.message

