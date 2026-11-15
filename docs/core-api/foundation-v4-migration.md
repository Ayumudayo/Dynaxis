# 4.0 기초 범위로의 전환

기준점은 refactor 브랜치의 `8d48ccf`다. 이전 기능은 이 Git 이력에서 복원할 수 있다.

4.0.0은 파괴적 API 변경이다. worlds/provider, realtime/FPS, UDP/RUDP, Lua/native plugin과 hot reload, 관리 앱·확장 배포 제어를 제거했다. 관련 헤더/타깃/환경 변수/검사/문서도 더 이상 제공하지 않는다. 모든 소비자는 재빌드해야 한다.

채팅의 인증 principal, logical session lease, 마지막 room은 유지한다. 재접속은 world 소유자 정책을 거치지 않으며, 다른 정상 서버에서도 같은 lease/room을 복원한다. Redis room 정보가 없으면 lobby로 복구한다. 인증과 room 권한 검사는 유지된다.

LoginRes의 protobuf 필드 9(world_id)는 이름과 번호를 reserve했다. 다른 채팅 필드 번호와 14-byte framing은 유지한다. 게임과 UDP attach opcode는 제거했으며 기존 번호를 새 기능에 재사용하지 않는다. 삭제 opcode 요청은 거절된다.

최소 core와 전체 MSA package는 각각 다른 설치 prefix를 사용한다. `BUILD_CORE_EXTENSIONS=ON`은 현재 MSA 통합 범위이며 이전 확장 API 제공을 뜻하지 않는다. 최소 package는 `server_core_HAS_EXTENSIONS=OFF`다.

DB의 기존 사용자·룸·메시지·세션 데이터를 삭제하는 migration은 없다. 운영 Redis의 오래된 world 키를 자동으로 삭제하지 않는다. 제거된 기능의 환경 변수는 새 실행에 사용하지 않는다.
