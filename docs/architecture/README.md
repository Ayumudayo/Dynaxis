# Architecture Docs

이 디렉터리는 구조 정리 이후의 canonical architecture index다.

- `docs/architecture/build-test-ownership.md`
  - build/test manifest ownership과 top-level 진입점 요약
- [필수 코어와 MSA 채팅 서버의 경계](core-and-msa.md)
  - 계층별 소유권, 최소/전체 구성, 선택 기능과 실패 경계
- [전체 마이크로서비스 관리 도구 설계안](service-control-plane.md)
  - 기존 admin_app 재사용, 서비스 상태·작업·배포 경계와 단계별 완료 기준

기존 심층 설계 문서 자체는 한 번에 이동하지 않았다. 이번 턴에서는 기존 entrypoint를 깨지 않기 위해 원래 위치를 유지하고, canonical index만 이 경로로 추가했다.
