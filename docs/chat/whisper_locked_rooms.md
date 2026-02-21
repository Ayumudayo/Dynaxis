# 귓속말 및 잠금 방 설계 노트

## 귓속말 메시징 (2025-10 계획)
- 명령 형식: `/whisper <user> <message...>` 또는 `/w` 축약형 (클라이언트 입력 파서에서 처리).
- 전제 조건: 송신자·수신자 모두 로그인(state_.authed) 상태여야 함. 미로그인 세션은 `MSG_ERR(UNAUTHORIZED)` 반환.
- 대상 해석: `state_.by_user`에서 사용자명으로 매칭된 모든 세션에 귓속말 전송(멀티 로그인 대비). 동명이인 정책 TBD — 1차 구현은 사용자명이 고유하다는 가정.
- 서버 로그: `corelog::info("[whisper] sender=... target=... text=...")` 형태로 모든 요청을 기록. 민감 정보 취급이므로 운영 정책(마스킹/보관 기한) 별도 정의 필요.
- 메시지 전달: 수신자에게 `MSG_WHISPER_BROADCAST`, 송신자에게도 확인용 `MSG_WHISPER_BROADCAST`(`direction=outgoing`)를 돌려 UI에서 "to/from" 구분.
- 응답/오류: 대상 미존재, 자기 자신 귓속말, 빈 메시지 등은 `MSG_WHISPER_RES`로 사유를 명시.
- 클라이언트 표시: 로그 패널에 `[whisper to bob] ...`, `[whisper from alice] ...` 스타일로 강조 색상 적용.
- UI 상호작용: 사용자 리스트 클릭 시 즉시 귓속말을 여는 기능은 도입하지 않음(오작동 방지). 모든 귓속말은 명시적 명령으로만 전송.

## 잠금 방 (비밀번호 보호)
- 생성 흐름: `/create <room> --password <pw>` (신규 명령) 또는 `/join <room> <pw>` 입력 시 방이 없으면 생성하면서 비밀번호 설정.
- 상태 저장: DB/Redis 룸 메타데이터에 `password_hash`(Argon2id) 저장. 서버 state_.rooms에도 잠금 여부 플래그 포함.
- 입장 규칙: 비밀번호 미제공 또는 불일치 시 `MSG_ERR(FORBIDDEN)` 반환. 실패 로그를 남기고 재시도 횟수 제한(추후 정책) 고려.
- 사용자 목록: 잠금 방은 비회원 조회(`/who`)를 허용하지 않으며, devclient 좌측 패널에서는 `🔒 room-name` 형식으로 표시.
- 스냅샷/리스트: `/rooms` 응답에 잠금 여부 필드 포함(예: `room(l)`), 클라이언트는 이를 아이콘으로 변환.
- 명령 UX: `/join <room>`만 입력하면 일반 방, `/join <room> <password>` 입력 시 잠금 방 인증. 잘못된 비밀번호 시 로그 패널에 시스템 메시지로 안내.
- 보안 고려: 비밀번호 입력은 CLI에서 그대로 노출되므로 운영 시 보안 지침(타인 화면 보호 등) 안내 필요.

## 향후 체크리스트
- [ ] 새로운 opcode (`MSG_WHISPER_REQ/RES/BROADCAST`) 정의 및 `protocol.md` 갱신
- [ ] 서버 로그 포맷(`[whisper]`/`[locked-room]`) 표준화
- [ ] devclient 명령 파서 및 로그 컬러링 확장
- [ ] 잠금 방 관련 단위 테스트(`/join` 성공/실패, 목록 숨김)
- [ ] 운영 가이드: 귓속말 로그 보관 정책, 비밀번호 재설정 절차
