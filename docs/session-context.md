# 세션 컨텍스트 스냅샷 (Assistant 대화 요약)

마지막 업데이트: 2025-09-06

## 현재 방향성/결정 사항
- 명명: 특정 프로젝트명 금지(예: Knights). 범용 타깃/네임스페이스 사용.
- 우선순위: 구현은 보류, 설계 우선. 서버 코어(server_core) 먼저, 이후 CLI 기반 채팅 클라이언트(dev_chat_cli).
- 아키텍처: 모놀리식이 아닌 MSA. Gateway(TCP ingress) + Auth/Chat/Match/World/Presence 서비스.
- 프로토콜: 외부(클라↔Gateway)는 바이너리 프레이밍, 내부는 gRPC + 이벤트 버스 분리.
- CLI 요구: 입력부/출력부 확실 분리, 슬래시 명령(/connect 등), 자동완성/힌트 제공.

## 문서/산출물 현황
- server-architecture: docs/server-architecture.md (MSA 다이어그램/구성)
- msa-architecture: docs/msa-architecture.md (서비스 경계/통신/보안/관측성)
- core-design: docs/core-design.md (서버 코어 상세)
- protocol: docs/protocol.md (외부/내부 프로토콜)
- configuration: docs/configuration.md (설정/운영)
- naming-conventions: docs/naming-conventions.md (범용 명명 규칙)
- cli-client-design: docs/cli-client-design.md (CLI UX/자동완성 설계)

## 다음 할 일(중요도 순)
1) 서비스별 API(proto IDL) 초안: Auth/Chat/Presence의 gRPC 메서드/메시지 정의
2) Gateway 라우팅 규칙 상세: 인증 전/후 경로, 토큰 검증 위임
3) CLI 자동완성 스펙 확정: 후보 정렬/컨텍스트 기반 추천/힌트 포맷
4) 설정 스키마 고정: 필수/옵션, 핫 리로드 가능 항목 구분
5) 성능/안정성 목표치 설정: 동시 접속/지연/에러율 지표

## 열린 쟁점/결정 대기
- 메시지 버스 선택(NATS/Kafka/Redis Streams)
- 내부 보안 체계(mTLS 배포/CA 운영, RBAC 범위)
- CLI 구현 언어/라이브러리(replxx 등) 확정

## 재개 체크포인트(다음 세션 시작 시 확인)
- 이 파일의 "다음 할 일" 우선 1~2번 진행 여부 합의
- proto IDL 저장 위치/네임스페이스 결정(`proto/` 디렉터리 권장)
- Gateway 초기 MVP 범위 확정(인증/에코/채팅 라우팅)

---
이 문서는 대화 컨텍스트 복원을 위한 요약입니다. 업데이트 시 위 "마지막 업데이트"만 갱신하세요.
