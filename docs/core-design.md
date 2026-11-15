# 코어 설계

코어는 연결, framing, handler dispatch, 실행 queue, scheduler, buffer, 종료와 관측을 제공한다. 업무 정책은 앱에서 정한다. [계층과 실행 구조](architecture/core-and-msa.md)를 기준으로 한다.

송신 예산은 executor 대기·write queue·진행 중 write의 메모리를 모두 계산한다. worker는 작업별 예외를 격리하며 Stop은 accepted queue를 drain한다. scheduler는 실패한 callback만 취소한다. pooled buffer는 pool 수명을 공유한다.

소유자는 I/O 문맥을 timer/socket/strand보다 오래 유지하고, ChatService 파괴 전에 구독과 worker를 종료한다. 각 runtime의 의존성은 명시적으로 주입한다.
