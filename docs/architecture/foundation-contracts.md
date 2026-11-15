# 기초 런타임과 채팅 계약

이 문서는 기초 영역이 실제로 보장하는 상태·전달·실패 규칙을 정한다. 기능 추가와 관리 콘솔은 이 계약의 회귀 검증을 유지하면서 진행한다.

## 코어와 실행 순서

`JobQueue`는 생산자의 공간 대기와 소비자의 작업 대기를 분리한다. `Stop()`은 양쪽 대기를 깨우고 이미 받은 작업은 소비자가 비운다.

채팅 명령과 연결 종료는 세션별 FIFO에서 실행한다. 여러 세션은 기존 worker pool을 공유한다. 세션별 대기 명령은 128개, 전체 대기는 설정한 job queue 크기(무제한 설정이면 4096개)로 제한한다. 포화는 `SERVER_BUSY`로 응답하며 close 정리는 버리지 않는다. I/O thread 기동 뒤 실패해도 채팅 객체를 해제하기 전에 shutdown과 join을 끝낸다.

## 공유 상태의 권한

Postgres의 `chat_shared_state`와 트랜잭션 잠금이 방 owner·초대·참여 권한·principal 로그인·제재·차단 목록의 기준이다. Redis의 TTL이나 특정 노드의 메모리 map이 권한을 새로 부여하지 않는다. 방 키의 대소문자 처리는 Postgres의 기존 방 이름 검색과 일치시킨다. owner는 방 생성 시에만 정하며 빈 방이나 프로세스 재시작으로 다른 참가자에게 양도하지 않는다. 명시적인 방 제거가 방의 세대를 끝낸다.

활성 연결은 principal당 하나다. 로그인 예약은 DB 시계 기준 120초 lease이며 명령과 ping으로 갱신한다. 정상 close는 자신이 소유한 예약만 해제한다. 유효한 resume 자격은 같은 logical session의 예약을 교체할 수 있고 이전 연결은 소유 토큰 검증에서 차단된다. 새 연결이 만든 presence를 이전 연결의 close가 지우지 않는다. 죽은 노드의 일반 로그인 예약은 lease 만료 뒤 회수할 수 있다.

권한 저장소 조회 실패를 허용으로 바꾸지 않는다. 요청은 재시도 가능한 오류를 받는다. 제재·kick·방 제거의 Pub/Sub 알림은 빠른 반영을 돕고, 다음 요청의 공유 상태 검증이 권한을 다시 확인한다. 로컬 캐시는 표시와 현재 연결 관리에 사용한다.

기존 DB에 소유권의 근거가 없는 방은 migration에서 owner 없는 상태로 보존한다. 첫 접속자에게 owner를 주지 않으며 관리자가 초대하거나 제거 후 재생성해야 한다. 기존 메시지를 삭제하는 migration은 아니다.

## 메시지 확정과 복구

채팅 본문은 server가 Postgres에 직접 저장한다. Redis Streams와 worker는 session event 적재 경로이며 채팅 본문의 전달 보증을 대신하지 않는다.

방별 DB writer 잠금을 **ID 할당 전부터 commit까지** 유지한다. 같은 방의 ID는 commit 순서를 나타낸다. commit 확인 후에만 그 ID로 캐시와 로컬·원격 전달을 수행한다. commit 실패나 결과 불명 시 확정 broadcast를 보내지 않는다. 연결 단절로 commit 결과가 불명일 수 있으므로 클라이언트는 refresh로 확인한 뒤 재전송을 결정한다. 자동 재전송의 exactly-once를 약속하지 않는다.

DB commit과 Redis publish는 하나의 트랜잭션이 아니다. 실시간 Pub/Sub은 best effort이며 장애 중 메시지 보존을 보장하지 않는다. 저장된 메시지는 snapshot의 최근 창 안에서 복구한다. 방별 ID 순서와 네트워크 도착 순서도 다를 수 있다. 클라이언트는 ID 집합으로 중복을 제거하고 늦게 도착한 작은 ID도 정렬해 병합한다.

snapshot은 DB writer와 같은 잠금 아래 watermark와 최근 최대 20개 메시지를 읽는다. watermark는 읽음 위치 `last_seen_id`와 다르다. 캐시 fallback은 부분 이력으로 표시하며 DB의 완전한 경계라고 광고하지 않는다. 전체 기간 replay나 무제한 이력 복구는 현재 계약에 없다. 상세 wire 계약은 [snapshot](../protocol/snapshot.md)을 따른다.

## 준비 상태와 검증

gateway readiness에는 Redis와 라우팅 가능한 backend가 필요하다. 채팅 server는 DB·Redis와 설정한 Pub/Sub 구독 확인이 필요하다. Redis PING 성공만으로 복구 완료를 선언하지 않는다.

회귀 검사는 MPMC 큐, 시작 실패, commit 실패, 실제 worker 순서, 다중 노드 권한·ID·DB rollback, backend 부재와 Redis/DB 장애 복구를 포함한다. 실행 경로는 [테스트 안내](../tests.md)에 있다. macOS 실기 검증과 데이터 계층 HA는 현재 Windows/Linux 기초 검증 범위 밖이다.
