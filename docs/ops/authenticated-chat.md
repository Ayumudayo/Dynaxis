# 인증된 MSA 채팅 실행

핵심 경로는 HAProxy → gateway → 채팅 server이며, Redis discovery/fanout/continuity와 Redis Streams → worker → Postgres 적재를 함께 사용한다. 인증은 `infra`의 `chat_auth` adapter가 담당하며 최소 `server_core`에는 인증·DB·Redis 의존성을 넣지 않는다.

## 인증 모드와 신원

- 독립 `gateway_app`, `server_app`의 기본은 `CHAT_AUTH_MODE=signed`다. `CHAT_AUTH_SIGNING_SECRET`이 32..4096 UTF-8 byte가 아니면 시작을 거절한다. gateway와 server는 같은 신뢰 key를 사용한다.
- Docker 로컬 검증 compose는 `CHAT_AUTH_MODE=development`를 명시적으로 기본 선택한다. 기존 guest/직접 로그인·continuity 시나리오를 위한 호환 모드다. `ALLOW_ANONYMOUS=0`만으로 서명 인증이 활성화되지는 않는다.
- signed 모드에서 username 입력은 신원을 결정하지 않는다. 검증된 `subject`를 routing, 채팅 identity, 저장소 사용자 이름과 제재 대상으로 사용한다. subject는 소문자 ASCII 식별자여서 대소문자 DB 비교로 다른 신원이 합쳐지지 않는다.
- `display_name`은 표시용이며 같은 표시 이름을 가진 credential도 서로 다른 subject다. 현재 귓속말/운영 명령의 대상은 `effective_user`로 반환한 subject다.
- `auth_session_id`는 credential 발급 식별자다. 채팅의 `logical_session_id`, `resume_token`, DB 사용자 UUID와 다른 값이다.

## 자격 발급과 실행

신뢰하는 운영 환경의 offline issuer가 자격을 발급한다. 별도 인증 프로세스나 공개 발급 API를 추가하지 않는다. 비밀번호 로그인, 회원가입, 외부 identity provider는 이 서버 코어의 기능으로 가정하지 않는다.

```powershell
$env:CHAT_AUTH_MODE = 'signed'
$env:CHAT_AUTH_SIGNING_SECRET = [Convert]::ToHexString([Security.Cryptography.RandomNumberGenerator]::GetBytes(32))
New-Item -ItemType Directory -Force tasks/credentials | Out-Null
python tools/issue_chat_token.py --subject account-001 --display-name '사용자 표시 이름' --ttl 900 --output tasks/credentials/account-001.token
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build
```

issuer는 자격을 지정한 새 파일에만 저장하고 stdout에 출력하지 않는다. 이미 있는 파일은 덮어쓰지 않는다. 자격 파일은 저장소/로그/공개 산출물에 포함하지 않는다. key는 환경에서 주입하고 클라이언트에 전달하지 않는다.

개발 클라이언트는 로그인 창의 `Access token`에 자격을 입력한다. `Resume previous session`은 마지막으로 서버에서 받은 continuity 자격을 함께 보낸다. access 자격은 인증을, resume 자격은 채팅 상태 복원을 담당한다. resume만으로 signed 인증을 대신할 수 없다. 클라이언트는 두 종류의 session 정보를 구분하고 resume 자격을 로그에 출력하지 않는다.

## 검증 계약

access 형식은 `chat1.<hex JSON>.<hex HMAC-SHA256>`이다. 서명 대상은 앞의 prefix와 payload 전체다. 고정 issuer/audience/version, subject/session 식별자, UTF-8 표시 이름, 발급/만료 시각과 최대 24시간 TTL을 검증한다. 서명은 일정 시간 비교를 사용하고 input 크기를 제한한다. 기본 발급 TTL은 900초다.

`MSG_LOGIN_REQ`의 기존 token 필드로 access를 전송한다. 재접속은 `access|resume:opaque-resume-token`으로 두 자격을 전송한다. gateway는 access를 검증한 뒤 backend를 선택하고 원래 프레임을 전달한다. server도 access를 독립 검증하므로 backend 직접 접속으로 인증을 우회할 수 없다. resume lease의 effective identity가 검증된 subject와 다르면 거절한다.

`LoginRes`의 기존 필드는 유지하며 `principal_id=10`, `auth_session_id=11`, `display_name=12`를 추가했다. 기존 protobuf decoder는 이 필드를 건너뛸 수 있다. 로그인 IP는 공개 채팅 메시지/history에 기록하지 않는다.

이 자격은 bearer credential이다. 동일 자격의 재사용은 허용되며 로그인 시점의 만료를 검사한다. 연결 중 만료에 따른 강제 종료, 중앙 revocation, refresh, 여러 key의 무중단 rotation은 제공하지 않는다. key 변경은 기존 key로 발급한 자격의 다음 로그인을 거절한다. 외부 전송 구간의 암호화와 issuer key 보관은 배포 환경에서 구성한다.

## 실행/복구 경계

실제 server bootstrap은 의존성을 instance context와 `ChatService::Dependencies`에 주입한다. 전역 Registry 기반 생성자는 기존 코드 호환을 위해 남지만 실제 bootstrap과 관측 HTTP는 이를 조회하지 않는다. chat 파괴 전에 worker를 drain하는 소유권 규약은 유지한다.

DB/Redis readiness 검사는 5초마다 bounded queue에 예약한다. 큐 적재 실패나 검사 예외는 준비 상태 실패로 반영한다. `ThreadManager`는 작업 예외를 기록하고 다음 작업을 계속 실행한다. 종료 시 새 enqueue를 거절하고 접수한 작업을 drain한 뒤 join한다. 실행 중인 작업을 강제 중단하지 않으므로 작업 자체의 I/O timeout도 필요하다.

Redis 구독은 재연결 자체가 실패한 경우에도 계속 재시도한다. 구독 스레드가 소켓을 단독 소유하고 250ms의 읽기 timeout으로 종료를 관측한다. 이 timeout은 유휴 구독을 해제하지 않는다. Pub/Sub은 장애 중 메시지 보존을 보장하지 않으며, Streams 적재 복구와 구분한다. bootstrap의 `io_context`는 이를 참조하는 timer/strand/acceptor와 instance context보다 오래 유지한다.

검증 진입점:

- `ctest --test-dir build-windows -C Debug -R "SessionTokenTest|ThreadManagerTests|ChatServiceTest" --output-on-failure --no-tests=error`
- `python tests/python/verify_signed_chat.py --manage-stack`: 현재 이미지를 signed 모드로 실행해 신원·거절·노드 간 전달·재접속 binding을 검증하고 원래 환경으로 복구한다.
- `python tests/python/verify_core_dependency_recovery.py`: 실행 중인 기본 개발 스택에서 DB/Redis를 실제 중지해 readiness와 앱 재시작 없는 복구를 검사한다. 운영 스택에 실행하지 않는다.
- `python tests/python/verify_continuity_recovery_matrix.py --no-build`: gateway/server 재시작 및 locator fallback/다른 서버 failover 검증.

현재 기초 실행 범위는 `docs/architecture/core-and-msa.md`에 정의한다.
