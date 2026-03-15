# Opcode 목록

기준 원본: `core/protocol/system_opcodes.json`, `server/protocol/game_opcodes.json`.
`tools/gen_opcode_docs.py`로 생성됩니다. 직접 수정하지 마세요.

## 시스템(Core)

- 원본: `core/protocol/system_opcodes.json`
- 네임스페이스: `server::core::protocol`

### system (0x0001..0x000F)

코어/시스템 프레임

| ID | 이름 | 방향 | 상태 | 처리 위치 | 전송 | 전달 보장 | 채널 | 설명 |
|---:|------|:---:|:-----:|:---------:|:----:|:---------:|-------:|------|
| 0x0001 | `MSG_HELLO` | `s2c` | `any` | `inline` | `tcp` | `reliable_ordered` | 0 | 버전/서버 정보 |
| 0x0002 | `MSG_PING` | `c2s` | `any` | `inline` | `both` | `reliable` | 0 | 하트비트 핑 |
| 0x0003 | `MSG_PONG` | `c2s` | `any` | `inline` | `tcp` | `reliable_ordered` | 0 | 하트비트 퐁 |
| 0x0004 | `MSG_ERR` | `s2c` | `any` | `inline` | `tcp` | `reliable_ordered` | 0 | 에러 응답 |

## 게임(Server)

- 원본: `server/protocol/game_opcodes.json`
- 네임스페이스: `server::protocol`

### auth (0x0010..0x001F)

로그인/인증

| ID | 이름 | 방향 | 상태 | 처리 위치 | 전송 | 전달 보장 | 채널 | 설명 |
|---:|------|:---:|:-----:|:---------:|:----:|:---------:|-------:|------|
| 0x0010 | `MSG_LOGIN_REQ` | `c2s` | `any` | `inline` | `tcp` | `reliable_ordered` | 0 | 로그인 요청 |
| 0x0011 | `MSG_LOGIN_RES` | `s2c` | `any` | `inline` | `tcp` | `reliable_ordered` | 0 | 로그인 응답 |
| 0x0012 | `MSG_UDP_BIND_REQ` | `c2s` | `any` | `inline` | `udp` | `reliable` | 0 | UDP 바인딩 요청 |
| 0x0013 | `MSG_UDP_BIND_RES` | `s2c` | `any` | `inline` | `both` | `reliable` | 0 | UDP 바인딩 응답/티켓 |

### chat (0x0100..0x01FF)

룸 채팅

| ID | 이름 | 방향 | 상태 | 처리 위치 | 전송 | 전달 보장 | 채널 | 설명 |
|---:|------|:---:|:-----:|:---------:|:----:|:---------:|-------:|------|
| 0x0100 | `MSG_CHAT_SEND` | `c2s` | `authenticated` | `inline` | `tcp` | `reliable_ordered` | 0 | 채팅 전송 |
| 0x0101 | `MSG_CHAT_BROADCAST` | `s2c` | `any` | `inline` | `tcp` | `reliable_ordered` | 0 | 채팅 브로드캐스트 |
| 0x0102 | `MSG_JOIN_ROOM` | `c2s` | `authenticated` | `inline` | `tcp` | `reliable_ordered` | 0 | 룸 입장 |
| 0x0103 | `MSG_LEAVE_ROOM` | `c2s` | `authenticated` | `inline` | `tcp` | `reliable_ordered` | 0 | 룸 퇴장 |
| 0x0104 | `MSG_WHISPER_REQ` | `c2s` | `authenticated` | `inline` | `tcp` | `reliable_ordered` | 0 | 귓속말 요청 |
| 0x0105 | `MSG_WHISPER_RES` | `s2c` | `any` | `inline` | `tcp` | `reliable_ordered` | 0 | 귓속말 응답 |
| 0x0106 | `MSG_WHISPER_BROADCAST` | `s2c` | `any` | `inline` | `tcp` | `reliable_ordered` | 0 | 귓속말 전달 |

### state (0x0200..0x02FF)

스냅샷/갱신

| ID | 이름 | 방향 | 상태 | 처리 위치 | 전송 | 전달 보장 | 채널 | 설명 |
|---:|------|:---:|:-----:|:---------:|:----:|:---------:|-------:|------|
| 0x0200 | `MSG_STATE_SNAPSHOT` | `s2c` | `any` | `inline` | `tcp` | `reliable_ordered` | 0 | 상태 스냅샷(방 목록+현재 방 유저) |
| 0x0201 | `MSG_ROOM_USERS` | `s2c` | `any` | `inline` | `tcp` | `reliable_ordered` | 0 | 특정 방 유저 목록 응답 |
| 0x0202 | `MSG_ROOMS_REQ` | `c2s` | `authenticated` | `inline` | `tcp` | `reliable_ordered` | 0 | 방 목록 요청 |
| 0x0203 | `MSG_ROOM_USERS_REQ` | `c2s` | `authenticated` | `inline` | `tcp` | `reliable_ordered` | 0 | 특정 방 사용자 목록 요청 |
| 0x0204 | `MSG_REFRESH_REQ` | `c2s` | `authenticated` | `inline` | `tcp` | `reliable_ordered` | 0 | 현재 방 스냅샷 요청 |
| 0x0205 | `MSG_REFRESH_NOTIFY` | `s2c` | `any` | `inline` | `tcp` | `reliable_ordered` | 0 | 상태 변경 알림 (클라이언트가 REFRESH_REQ를 보내도록 유도) |
