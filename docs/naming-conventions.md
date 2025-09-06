# 명명/네임스페이스 가이드

## 원칙
- 레포/코드/바이너리/네임스페이스에 특정 프로젝트명(예: Knights)을 사용하지 않는다.
- 범용적이고 역할 중심의 이름을 사용한다.

## 네임스페이스/타깃명
- C++ 네임스페이스: `server::core`, `server::gateway`, `server::auth`, `server::chat`, `server::match`, `server::world` 등 역할 기반.
- CMake 타깃: `server_core`(lib), `gateway_service`(exe), `auth_service`(exe), `chat_service`(exe), `match_service`(exe), `world_service`(exe), `dev_chat_cli`(exe), `server_tests`(exe).
- 바이너리 파일명: 서비스명과 동일하게 유지.

## 디렉터리/파일
- include: `core/include/server/core/...`
- 소스: `core/src/...`, `services/<service>/src/...`, `devclient/src/...`
- 문서: `docs/`

## 코드 스타일(요약)
- C++20. 파일 상단 `#pragma once`. snake_case 파일명 권장.
- 클래스/타입: PascalCase, 함수/변수: snake_case.
- 비동기 콜백은 명확한 접두사(`do_read`, `do_write`).

## CLI 명령 네이밍
- 프리픽스 `/` 필수. 예: `/connect`, `/login`, `/join`, `/say`.
- 인자 표기: `<필수>`, `[옵션]`, 가변은 `...` 사용. 예: `/say <message...>`
- 알리어스는 최소화, 명확성 우선. 예: `/whisper`만 유지(`/w` 비권장).
