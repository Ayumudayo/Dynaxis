# dev_chat_cli

`devclient/` 는 FTXUI 로 작성된 개발자용 CLI 입니다. Gateway 에 TCP 로 접속해 로그인/방 이동/귓속말/스냅샷 등을 빠르게 검증할 수 있습니다.

## 구조
```
devclient/
├─ include/client/app/
│  ├─ app_state.hpp, network_router.hpp 등 UI/상태 관리
└─ src/
   ├─ app/ (UI + 명령 파서)
   ├─ ui/  (FTXUI 컴포넌트)
   └─ net_client.cpp (wire codec)
```

## 주요 기능
- `/login`, `/join`, `/chat`, `/whisper` 명령 지원
- 스크롤 로그, 방/사용자 목록, 자동 새로고침
- `Ctrl+C` 종료 시 `/leave` 를 자동 전송해 서버에 정상 종료를 알립니다.

## 환경 변수
| 이름 | 설명 | 기본값 |
| --- | --- | --- |
| `DEVCLIENT_HOST` | 접속할 Gateway 주소 | `127.0.0.1` |
| `DEVCLIENT_PORT` | Gateway 포트 | `6000` |

`.env.devclient` 를 두고 `ENV_FILE` 로 지정하면 다중 환경 전환이 쉽습니다.

## 빌드 & 실행
```powershell
cmake --build build-msvc --target dev_chat_cli
.uild-msvc\devclient\Debug\dev_chat_cli.exe
```
FTXUI 는 vcpkg 패키지를 사용하므로, `scripts/build.ps1` 실행 시 `-UseVcpkg` 옵션을 붙이는 것을 권장합니다.
