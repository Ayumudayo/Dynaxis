# dev_chat_cli (Developer Client)

`devclient/` 모듈은 FTXUI 기반 터미널 클라이언트(`dev_chat_cli`)를 제공하여 Gateway ↔ Load Balancer ↔ Server 경로를 수동 검증할 때 사용합니다. 기본 사용자 흐름은 익명 로그인 후 `/join`, `/leave`, `/whisper` 등 서버 명령을 테스트하도록 맞춰져 있습니다.

## 디렉터리 구조
- `include/client/app/` — UI 상태, 명령 처리기, 네트워크 라우터 인터페이스
- `src/app/` — FTXUI 화면 구성, 이벤트 루프, 명령 처리 구현
- `src/ui/` — 상태 바 등 UI 컴포넌트 모듈
- `src/net_client.cpp` — wire 프로토콜 TCP 클라이언트 구현
- `src/main.cpp` — 엔트리 포인트
```text
devclient/
├─ include/
│  └─ client/
│     └─ app/
├─ src/
│  ├─ app/
│  ├─ ui/
│  ├─ net_client.cpp
│  └─ main.cpp
```


## 환경 변수
| 변수 | 설명 | 기본값 |
| --- | --- | --- |
| `DEVCLIENT_HOST` | 연결할 Gateway 호스트 | `127.0.0.1` |
| `DEVCLIENT_PORT` | 연결할 Gateway 포트 | `6000` |

`.env`는 실행 파일과 같은 디렉터리 또는 리포지터리 루트에서 자동으로 찾아 로드합니다.

## 빌드 & 실행
```powershell
cmake --build build-msvc --target dev_chat_cli
.\build-msvc\devclient\Debug\dev_chat_cli.exe
```
FTXUI 종속성은 `vcpkg` 매니페스트에 정의되어 있으므로 루트에서 `cmake --preset` 혹은 `scripts/build.ps1`을 실행하면 자동으로 설치됩니다.

## 사용 팁
- `/login <name>` 입력 후 `/join lobby`로 채팅 룸에 입장합니다.
- 익명 상태에서는 임의 닉네임으로 로그인되며, `/refresh`, `/rooms`, `/who` 등의 명령으로 상태를 즉시 확인할 수 있습니다.
- 종료는 `Esc` 또는 `Ctrl+C`로 수행하며, 클라이언트가 Gateway에 graceful `/leave` 메시지와 `socket shutdown`을 전송하도록 구성되어 있습니다.

향후 UI 개선이나 자동 시나리오 테스트가 필요하다면, `CommandProcessor`와 `NetworkRouter`를 확장하여 스크립팅 기능을 추가하세요.
