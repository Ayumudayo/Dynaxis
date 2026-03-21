# 발견(Discovery) API 가이드

## 안정성

| 헤더 | 안정성 |
|---|---|
| `server/core/discovery/instance_registry.hpp` | `[Stable]` |
| `server/core/discovery/world_lifecycle_policy.hpp` | `[Stable]` |

## 기준 이름

- 기준 include 경로: `server/core/discovery/**`
- 기준 네임스페이스: `server::core::discovery`
- `server/core/state/**`는 underlying implementation/integration 경로이며 consumer가 직접 의존해야 하는 안정 표면이 아니다

이 구분이 필요한 이유는 public 이름과 내부 구현 경로를 분리해야 하기 때문이다. 그렇지 않으면 지금은 편해 보여도 concrete adapter나 내부 캐시 전략이 그대로 public ABI처럼 굳어 버린다.

## 범위

- shared instance record
- selector 로직
- backend interface
- in-memory backend
- world lifecycle policy serialization/parsing

여기서 멈추는 이유도 중요하다. discovery는 “공용 상태를 어떻게 표현하고 고르는가”까지를 책임지고, 실제 Redis/Consul adapter나 sticky routing 전략까지 책임지지 않는다.

## 공개 계약

- canonical `discovery/**` 헤더는 underlying `state/**` ownership 위에 올린 public facade입니다. consumer는 `discovery/**`를 기준 표면으로 보고, `state/**`를 동등한 stable API로 간주하면 안 됩니다.
- `InstanceRecord`
  - 라우팅과 control-plane inventory에 쓰는 공유 인스턴스 스냅샷이다.
- `InstanceSelector`
  - 역할, game mode, region, shard, tag 기준으로 인스턴스 집합을 좁히는 필터다.
- `SelectorPolicyLayer`
  - selector가 어느 정책 계층까지 좁혀졌는지 표현한다.
- `SelectorMatchStats`
  - selector 적용 과정에서 몇 개를 스캔했고, 몇 개가 매치/미스매치했는지 기록한다.
- `matches_selector()`, `classify_selector_policy_layer()`, `selector_policy_layer_name()`, `select_instances()`
  - selector/filter 동작을 정의하는 helper다.
- `IInstanceStateBackend`
  - discovery store가 따라야 하는 최소 backend 경계다.
- `InMemoryStateBackend`
  - 테스트나 단일 프로세스 환경에서 쓰는 안정적인 기본 backend다.
- `WorldLifecyclePolicy`, `parse_world_lifecycle_policy()`, `serialize_world_lifecycle_policy()`
  - world drain/replacement owner 정책을 공유 문서 형태로 표현하는 계약이다.

## 비목표

- Redis-backed registry construction contract
- Consul adapter contract
- sticky `SessionDirectory` contract
- gateway 전용 least-connections / sticky-routing 정책

이것들을 일부러 제외하는 이유는, discovery의 공용 개념과 gateway의 app-owned routing 전략을 섞지 않기 위해서다. 둘을 섞으면 public surface가 지나치게 구체화되어 다른 consumer가 재사용하기 어려워진다.

## 공개 검증

- public-api smoke/header scenario는 discovery 헤더를 직접 포함한다
- installed consumer proof: `CoreInstalledPackageConsumer`
- focused stable discovery proof:
  - `tests/state_instance_registry_tests.cpp`
- app-owned boundary note:
  - `tests/gateway_session_directory_tests.cpp`는 sticky `SessionDirectory` 동작을 검증하는 gateway-side proof이며, stable discovery surface 자체의 proof로 보지 않는다
