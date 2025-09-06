# 서버 코어 상세 설계

## 목적/범위
- 목적: 범용 TCP 기반(Windows IOCP/Asio) 서버 코어를 제공한다.
- 범위: 네트워크 I/O, 프레이밍/코덱, 디스패치, 타이머, 기초 로깅/메트릭. 게임/도메인 로직은 외부 모듈.

## 상위 구성요소
- Net
  - Acceptor: 리슨/accept 루프, 연결 제한, IP 필터, 재시작.
  - Session: 비동기 read/write, 프레이밍, 송신 큐, 타임아웃, heartbeat.
  - Transport/Codec: 바이트 스트림 ↔ 메시지 패킷 변환.
- Dispatch
  - Dispatcher: `msg_id -> handler` 매핑, 핸들러 실행 컨텍스트(executor/strand) 관리.
- Utils
  - Timer: 고정 틱/지연 실행, cancel-safe.
  - Log/Metrics: 최소한의 인터페이스(구현은 교체 가능).

## 스레딩/실행 모델
- I/O: `io_context` 스레드 풀(N=코어 수 권장)에서 모든 비동기 콜백이 수행.
- Session 직렬화: 세션별 `strand` 또는 메시지 큐로 논리 상태 일관성 보장.
- 핸들러 실행: 기본은 같은 `io_context`에서 실행하되, 룸/존 단위 분리 시 전용 executor를 주입.

### 세션 직렬화 선택지
- 기본: 세션별 직렬화는 `strand`로 보장. 동일 세션의 read/write/핸들러가 중첩 실행되지 않게 함.
- 대안: lock-free MPSC 큐를 두고 `io_context.post`로 순차 실행. 고부하 시 튜닝 포인트.

## 패킷/프레이밍
- Header(v1.1):
  - `uint16 length`(본문 길이) + `uint16 msg_id` + `uint16 flags` + `uint32 seq` + `uint32 utc_ts_ms32` = 14바이트
  - Endianness: network(big-endian).
- 본문: 메시지별 바이너리 스키마(초기 수제, 확장 시 Protobuf/FlatBuffers 선택).
- Heartbeat: `MSG_PING`, `MSG_PONG` 예약. 무응답 시 세션 종료.
- 문자열: UTF-8 고정, 길이-접두(프리픽스) 방식 사용.

### 프레이밍/코덱 플러그인 SPI
- CodecChain 인터페이스(제안): `encode(payload) -> bytes`, `decode(bytes) -> payload`.
- 체인 예: [암호화] → [압축] → [헤더부착]. 구성은 설정 파일로 지정.
- 압축 기준: payload >= N바이트 시 시도, 효과 없으면 원본 유지 + `flags` 비트 클리어.

## API 스케치(C++)
```cpp
namespace server::core {

class Dispatcher {
public:
  using handler_t = std::function<void(Session&, std::span<const std::byte>)>;
  void register_handler(uint16_t msg_id, handler_t h);
  bool dispatch(uint16_t msg_id, Session& s, std::span<const std::byte> payload);
};

class Session : public std::enable_shared_from_this<Session> {
public:
  void start();
  void stop();
  void async_send(uint16_t msg_id, std::span<const std::byte> payload);
  asio::ip::tcp::socket& socket();
};

class Acceptor : public std::enable_shared_from_this<Acceptor> {
public:
  void start();
  void stop();
  std::size_t connection_count() const;
};

} // namespace
```

## 연결/흐름 제어 정책
- 동시 연결 상한: 설정값 초과 시 즉시 종료.
- 수신 크기 제한: `length` 임계치 초과 시 드롭.
- 송신 큐 상한: 메시지 수/바이트 기준 임계치 초과 시 세션 종료(백프레셔).
- 레이트 제한: IP별/세션별 토큰 버킷(선택)으로 남용 방지.

### 송신 큐 워터마크
- low/high watermark를 두어 배압을 점진적으로 적용.
- high 초과: 세션 종료 또는 저우선 메시지 드롭 정책 적용.

## 에러 모델/로깅
- 오류 표준: `boost::system::error_code` 사용. 범주: network, parse, policy, internal.
- 로깅 레벨: trace/debug/info/warn/error. PII 최소화.
- 관측성: 핵심 카운터(accept 실패, 세션 수, 큐 길이, 처리 시간 p50/p99).

### 메트릭 세부
- net.accept.success/fail, net.accept.active_sessions
- net.recv.bytes, net.send.bytes, net.send.queue_len
- net.read.timeout, net.write.timeout, net.heartbeat.miss
- dispatch.missing_handler, dispatch.handler_error

## 종료/재시작
- Graceful: accept 중단 → 신규 세션 차단 → 기존 세션 드레이닝/타임아웃 → 자원 해제.
- Force: 타임아웃 초과 또는 관리자 명령 시 강제 종료.

## 세션 상태 머신/타임아웃
- 상태: `New -> Active -> Draining -> Closed`
  - New: 핸드셰이크/인증 전. 제한된 메시지만 허용.
  - Active: 정상 운영. 핑-퐁/채널 조작/메시지 전송 허용.
  - Draining: 종료 절차 중. 신규 수신 거부, 송신 큐 소진 후 종료.
  - Closed: 소켓 종료/리소스 해제.
- 타임아웃: `read_timeout`, `write_timeout`, `heartbeat_interval * miss_threshold`.
- 하트비트: 서버 또는 클라 주도 가능. 서버 주기 T, 미수신 K회 → 종료.

## 테스트 전략
- Session 단위: 프레이밍/부분 읽기/대형 패킷/타임아웃.
- Dispatcher: 중복 등록/미등록/예외 전파.
- Acceptor: 빠른 open/close 반복, 연결 폭주 시 정책 확인.
- 부하: 수천 세션 echo/브로드캐스트 시나리오로 지연·스루풋 측정.

### 핸들러 계약/예외
- 핸들러는 예외를 던지지 않는 것을 원칙으로 하나, 발생 시 Dispatcher가 잡아 로깅하고 세션을 안전 종료.
- 핸들러 실행 시간 상한은 정책으로 모니터링. 장시간 블로킹 금지.

## 설정 매핑(코어 ↔ configuration.yaml)
- `server.io_threads` → `io_context` 워커 스레드 수
- `server.max_connections` → Acceptor 연결 상한
- `server.recv_max_payload` → 프레이밍 길이 상한
- `server.send_queue_max` → 송신 큐 바이트 상한/워터마크
- `server.heartbeat_interval_ms`/`read_timeout_ms`/`write_timeout_ms` → 타이머 파라미터
- `security.tls_*` → TLS 콘텍스트 초기화(선택)
- `logging.level` → 코어 로거 레벨

## 확장/교체 포인트
- Codec 체인: 압축/암호화 플러그 가능.
- 인증 훅: 최초 메시지 교환에서 인증 모듈 연계.
- Executor 주입: 룸/존 샤딩 시 외부 실행 컨텍스트 제공.
- 기본 파라미터(권장 값)
  - `io_threads`: CPU 코어 수, 최소 1.
  - `heartbeat_interval_ms`: 10_000ms, miss 3회.
  - `recv_max_payload`: 32KB, 최대 64KB.
  - `send_queue_max`: 256KB, low/high = 1MB/2MB(서비스 성격에 따라 조정).

## 성능/안정성 목표(초안)
- 지연: PING p50 < 2ms, p99 < 20ms(동일 AZ/로컬).
- 동시 세션: 단일 GW 인스턴스 20k 세션 안정 유지.
- 처리량: 채팅 브로드캐스트 10k msg/s에서 안정 동작.
- 오류율: 0.1% 이하(서킷브레이커/재시도 제외).
