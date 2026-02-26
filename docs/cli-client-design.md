# 개발용 CLI 클라이언트 설계

`devclient/`는 서버 통합 테스트를 돕는 FTXUI 기반 터미널 애플리케이션이다. 서버 코어와 동일한 프로토콜(14바이트 헤더)을 사용해 `server_app`에 접속하고, 로그인·방 이동·채팅·귓속말 등 채팅 기능을 검증한다.

## 실행 흐름
1. `client::app::Application`이 전체 수명주기를 담당한다.  
   - `AppState` 초기화 및 기본 로그 출력  
   - `NetClient`로 `127.0.0.1:5000`에 연결 시도 → 성공 시 게스트 로그인 요청 전송  
   - `NetworkRouter::Initialize()`로 모든 네트워크 콜백을 UI 스레드(Post 이벤트)와 연결  
   - `UiBuilder`가 FTXUI 레이아웃을 생성하고 입력 이벤트를 바인딩한다.  
2. 메인 루프는 `ftxui::ScreenInteractive::Fullscreen()`에서 돌아가며, ESC/Ctrl+C로 종료한다.  
3. 종료 시 네트워크 스레드를 정리하고 소켓을 닫는다.

## 주요 구성 요소
- **NetClient (`devclient/src/net_client.cpp`)**  
  - Boost.Asio TCP 소켓을 사용한다. 연결 후 server HELLO 프레임을 수신해 capability(`CAP_SENDER_SID`)를 파악한다.  
  - 수신 스레드와 8초 주기의 PING 송신 스레드를 별도로 운용한다.  
  - 수신한 opcode는 protobuf 디코더(`server/wire/codec.hpp`)를 통해 상위 구조체로 변환한 뒤, 등록된 콜백을 호출한다.  
  - 송신은 `send_frame_simple()`을 통해 헤더 + payload를 구성한다. 로그인/조인/채팅/귓속말 등 모든 명령은 길이-접두 UTF-8 인코딩을 사용한다.
- **AppState (`devclient/include/client/app/app_state.hpp`)**  
  - 사용자명, 현재 방, 최근 방 목록, 사용자 목록, 로그 버퍼(최대 1000줄) 등을 관리한다.  
  - 로그는 자동 스크롤 옵션과 선택 인덱스를 별도로 유지하며, 멀티스레드 접근을 위해 mutex를 사용한다.  
  - UI 동작(도움말 표시, 좌측 패널 폭, 입력 버퍼 등)을 저장한다.
- **CommandProcessor**  
  - 슬래시 명령을 파싱해 NetClient API를 호출한다. 현재 등록된 명령은 `/login`, `/join`, `/whisper`(alias `/w`), `/leave`, `/refresh`.  
  - 명령어 이외 입력은 일반 채팅으로 간주하며, 게스트 상태에서는 전송하지 않는다.
- **NetworkRouter**  
  - NetClient 콜백을 등록하고, `screen.Post()`를 통해 UI 이벤트 큐에서 안전하게 상태를 갱신한다.  
  - 시스템 브로드캐스트(`rooms:`), 스냅샷, 귓속말, 에러 응답을 해석해 `AppState`를 갱신하고 로그를 남긴다.  
  - 서버 capability(`sender_sid`)에 따라 자신의 메시지를 식별해 로그에 `me`로 표기한다.
- **UiBuilder**  
  - FTXUI 컴포넌트를 조립해 좌측(방/사용자), 우측(로그), 하단 입력창 레이아웃을 만든다.  
  - F1 → 도움말 오버레이, F5 → `/refresh`, ←/→ → 좌측 패널 너비 조절, Enter → 입력 전송 등의 단축키를 처리한다.  
  - 좌측 패널 방 목록을 이동하면 즉시 `/who` 요청을 전송해 해당 방 사용자 목록을 갱신한다.

## 사용자 경험(UI/UX) 요약
- **상태 표시줄**: 접속 여부 LED, 사용자명, 현재 방, 단축키 안내를 폭에 맞춰 동적으로 구성한다.
- **로그 패널**: 최신 메시지가 아래쪽에 표시되며, 스크롤 시 자동 스크롤이 해제된다. 시스템/귓속말/사용자 메시지를 텍스트로 구분한다.
- **도움말 오버레이**: F1 키로 주요 단축키와 명령 사용법을 토글한다.
- **입력 박스**: 단일 줄 입력, placeholder는 “메시지를 입력하세요…”. Enter 시 명령 처리 → 일반 메시지 전송 흐름으로 이어진다.

## 지원 기능
- 게스트 자동 로그인 → `/login <name>`로 닉네임 갱신. 서버에서 할당한 session id와 nickname을 로그로 표시한다.
- `/join <room> [password]`: 방 이동 및 비밀번호 지정, 서버 시스템 메시지를 로그로 출력. 입장 성공 시 스냅샷 응답으로 로그/사용자 목록 초기화.
- `/whisper <user> <message>`: 로그인 사용자 간 귓속말, 수신/발신 여부에 따라 로그 프리픽스가 다르다.
- `/leave [room]`: 지정 방 또는 현재 방 이탈 후 로비로 이동.
- `/refresh`: 현재 방 스냅샷과 `/rooms` 정보를 재요청한다. F5 단축키와 동일하다.
- 시스템 브로드캐스트 기반 기능  
  - `rooms:` 메시지를 파싱해 방 리스트 + 잠금 정보(`🔒`) 갱신  
  - `(system)` 방 메시지를 로그에 그대로 기록  
  - `MSG_ROOM_USERS`, `MSG_STATE_SNAPSHOT`을 이용해 UI 패널 최신화
- 오류 처리: `MSG_ERR` 수신 시 에러 코드/메시지를 로그에 출력하고, 인증/방 관련 에러에는 한글 힌트를 덧붙인다.

## 네트워크 세부
- HELLO → LOGIN_REQ → LOGIN_RES 순으로 초기 handshake가 이루어진다. 로그인 성공 시 자동으로 `/refresh`를 요청한다.
- 서버 capability에 따라 `sender_sid`(uint32) 비교로 본인 메시지를 식별하고, fallback으로 `FLAG_SELF`를 사용한다.
- `NetClient::close()`는 수신/핑 스레드를 안전하게 종료하기 위해 `running_` 플래그를 false로 설정한 후 join 한다.
- 모든 송신은 strand를 사용하지 않고 단일 mutex(`send_mu_`)로 보호한다.

## 향후 개선 항목
- 슬래시 명령 자동완성 및 히스토리 탐색 (현재 미구현).
- 연결 대상(host/port) 선택 UI, 환경 변수 기반 초기 설정.
- redis presence 등 외부 의존성 상태를 표시하는 추가 패널.
- 로그/명령에 대한 스크립트 실행 기능(매크로) 도입.

본 문서는 `devclient/`의 구현(2025-03 기준)을 반영한다. 추가 기능 개발 시 명령어 테이블, UI 단축키, NetClient 동작을 함께 갱신해야 한다.
