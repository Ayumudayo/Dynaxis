# Gateway & Load Balancer 가이드

목표: server_app 다중 인스턴스를 외부 L4/L7 로드밸런서 뒤에 두고 TLS 종료, 헬스체크, 드레인(무중단 롤링) 절차를 확립한다.

## 현재 상태 요약
- server_app가 게이트웨이 역할을 겸한다(클라이언트 TCP 프로토콜 수신, 분산 브로드캐스트 브릿지).
- 멀티 인스턴스 지원: `USE_REDIS_PUBSUB=1`, 고유 `GATEWAY_ID`로 self‑echo 방지.
- 헬스/드레인: TCP 연결 수립만으로 체크 가능. 종료 신호 시 Pub/Sub 구독 정리 로직 포함.

권장: LB/프록시는 외부 표준 솔루션(HAProxy, Nginx Stream, Envoy)을 사용하고, 프로젝트 내에서 직접 구현하지 않는다.

## 토폴로지
Client ↔ LB(HAProxy/Nginx/Envoy) ↔ server_app × N ↔ Redis/Postgres

## 환경 변수(요약)
- `USE_REDIS_PUBSUB=1` — 분산 브로드캐스트 활성화
- `GATEWAY_ID=gw-a` — 인스턴스 고유 ID
- `REDIS_CHANNEL_PREFIX=knights:` — Redis 키/채널 접두사
- `REDIS_URI=redis://host:6379` — Redis 연결

## HAProxy 샘플(tcp + TLS 종료)
```
global
  maxconn 4096

defaults
  mode tcp
  timeout connect 5s
  timeout client  60s
  timeout server  60s

frontend knights_tls
  bind *:443 ssl crt /etc/haproxy/certs/knights.pem
  default_backend knights_chat

backend knights_chat
  balance leastconn
  option tcp-check
  # 단순 TCP connect 체크
  server gw1 10.0.0.11:5000 check
  server gw2 10.0.0.12:5000 check
```

운영 팁
- 드레인: `echo "disable server knights_chat/gw1" | socat stdio /run/haproxy/admin.sock`
- 재편성 시 health-check 통과 상태만 트래픽 유입

## Nginx Stream 샘플(tcp + TLS 종료)
```
stream {
  upstream knights_chat {
    least_conn;
    server 10.0.0.11:5000 max_fails=2 fail_timeout=5s;
    server 10.0.0.12:5000 max_fails=2 fail_timeout=5s;
  }

  server {
    listen 443 ssl;
    ssl_certificate     /etc/nginx/certs/knights.crt;
    ssl_certificate_key /etc/nginx/certs/knights.key;
    proxy_connect_timeout 5s;
    proxy_timeout 60s;
    proxy_pass knights_chat;
  }
}
```

운영 팁
- 드레인: 대상 서버 weight=0으로 조정 후 reload(무중단), 예: `server gw1 10.0.0.11:5000 weight=0;`

## 드레인/롤링 배포 절차(권장)
1) LB에서 대상 인스턴스 드레인(gw disable 또는 weight=0)
2) 연결 소진 대기(세션 TTL 정책 고려; 기본 60s)
3) 프로세스 종료(Windows: Ctrl+C, Linux: SIGTERM)
4) 재기동 후 health 통과 확인 → LB에 재등록

참고: 서버는 종료 신호에서 Redis Pub/Sub 구독을 먼저 정리하여 중복/유실을 최소화한다.

## 헬스체크
- 최소: TCP connect 성공 여부로 판단(option tcp-check / 단순 connect)
- 선택: 향후 HTTP 헬스 포트 추가(예: `HEALTH_PORT` 노출, `/healthz` 200 OK) — 필요 시 구현 예정

## 스케일링 체크리스트
- 각 인스턴스 `GATEWAY_ID` 고유값 설정(중복 self‑echo 방지)
- 동일 `REDIS_CHANNEL_PREFIX` 유지(브로드캐스트 채널 정합)
- `PRESENCE_CLEAN_ON_START=0`(다중 게이트웨이 환경)

## 로컬 멀티 인스턴스 테스트(개요)
1) Redis 기동
2) 서버 두 개 실행: `GATEWAY_ID=gw-a`, `GATEWAY_ID=gw-b`
3) LB(HAProxy/Nginx) 앞단 구성 후 클라이언트 다중 접속
4) 같은 룸에서 채팅 → 상호 브로드캐스트 수신 확인

