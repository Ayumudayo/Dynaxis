# Core API 변경 이력

## 4.0.0 — 미출시

- 필수 TCP runtime과 MSA 채팅 기반을 유지한다.
- worlds/provider, realtime, UDP/RUDP, plugin/scripting API를 제거한다.
- 기존 consumer는 재빌드하고 제거 API 참조를 정리해야 한다.
- [파괴적 변경과 migration](foundation-v4-migration.md)

3.x의 구현과 변경 이력은 `8d48ccf` 기준점에서 확인한다.
