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

## 명령 & 단축키
| 명령 | 설명 |
| --- | --- |
| `/login <name>` | 로그인 및 세션 SID 확인 |
| `/join <room>` | 방 이동 |
| `/chat <text>` 또는 그냥 입력 | 채팅 메시지 전송 |
| `/whisper <user> <msg>` | 귓속말 |
| `/rooms`, `/who` | 방/사용자 목록 |
| `/refresh` | snapshot 다시 요청 |

키보드 단축키: `Ctrl+C` 또는 `Esc` 로 종료하면 `/leave` 를 자동으로 전송합니다. `PgUp/PgDn` 으로 로그 스크롤, `Tab` 으로 입력창 전환이 가능합니다.

## 로그
- 기본적으로 표준 출력으로 로그가 남고, `devclient.log` 파일(루트 디렉터리)에 최근 실행 기록을 append 합니다. 버그 제보 시 이 로그를 함께 첨부해 주세요.

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
