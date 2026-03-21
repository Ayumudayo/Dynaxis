# 월드 AWS API 가이드

## 안정성

| 헤더 | 안정성 |
|---|---|
| `server/core/worlds/aws.hpp` | `[Stable]` |

## 기준 이름

- 기준 include 경로: `server/core/worlds/aws.hpp`
- 기준 네임스페이스: `server::core::worlds`

## 범위

- generic topology / adapter lease / runtime-assignment 모델 위에 올라가는 AWS-first provider 해석 계약
- world pool 하나를 AWS/EKS 관점에서 읽기 위한 identity, load balancer, managed dependency, placement metadata 규칙

이 표면은 의도적으로 read-only contract다. 즉, “AWS에서 어떤 이름과 상태로 해석해야 하는가”를 설명하지, 실제 AWS SDK 호출이나 인프라 생성 루프를 제공하지는 않는다.

## 공개 계약

- `AwsLoadBalancerScheme`
  - `internal`
  - `internet-facing`
- `AwsLoadBalancerType`
  - `network`
  - `application`
- `AwsLoadBalancerTargetKind`
  - `ip`
  - `instance`
- `AwsPlacementTarget`
  - `region`
  - `availability_zones[]`
  - `subnet_ids[]`
- `AwsPoolIdentity`
  - `cluster_name`
  - `namespace_name`
  - `workload_name`
  - `service_name`
  - `iam_role_name`
- `AwsLoadBalancerAttachment`
  - service 이름, LB 이름, target group 이름, listener 정책을 묶는다
- `AwsManagedDependencyConventions`
  - managed Redis/Postgres naming convention 묶음이다
- `AwsAdapterDefaults`
  - provider-wide default cluster identity, placement, LB policy, dependency prefix를 담는다
- `AwsPoolBinding`
  - `KubernetesPoolBinding` 하나를 AWS-first provider binding으로 확장한 결과다
- `AwsLoadBalancerObservation`
  - LB attachment / target group / target health 관측값
- `AwsManagedDependencyObservation`
  - Redis/Postgres readiness 관측값
- `AwsPoolAdapterPhase`, `AwsPoolAdapterNextAction`
  - provider adapter가 현재 어느 단계에 있는지 표현한다
- `make_aws_pool_binding()`
  - generic `KubernetesPoolBinding`을 AWS identity, placement, LB naming, managed dependency naming으로 풀어낸다
- `evaluate_aws_pool_adapter_status()`
  - binding, optional lease/runtime assignment, LB/dependency observation을 합쳐 provider-facing status summary를 만든다

## 태그 규약

- 입력 seam은 기존 generic `placement_tags[]`를 그대로 사용한다
- AWS 해석 시 읽는 접두사:
  - `region:<value>`
  - `az:<value>` 또는 `zone:<value>`
  - `subnet:<value>`
  - `aws-lb-scheme:internal|internet-facing`
  - `aws-lb-type:nlb|alb`
  - `aws-lb-target:ip|instance`
- 태그가 없으면 `AwsAdapterDefaults`로 fallback한다

이 규약이 좋은 이유는 provider별 세부 정책을 generic desired topology 문서에 직접 박아 넣지 않아도 되기 때문이다. provider-specific 정보는 placement tag로 흘리고, 해석은 adapter contract에서 수행하는 쪽이 유지보수에 유리하다.

## 의미 규약

- `scale_out_pool` / `restore_pool_readiness`
  - LB attachment가 없으면 `await_load_balancer_attachment`
  - target group이나 target health가 준비되지 않았으면 `await_target_health`
  - managed Redis/Postgres 준비가 안 되었으면 `await_managed_dependencies`
  - provider 준비는 끝났지만 runtime assignment가 부족하면 `await_runtime_assignment`
  - 모두 만족하면 `complete`
- `scale_in_pool`
  - 현재 provider slice에서는 generic drain/orchestration surface가 teardown choreography를 담당하고, AWS 해석 계층은 `complete`로 본다
- `observe_undeclared_pool`
  - read-only 관찰 상태로 남긴다

## 비목표

- 직접 AWS API 인증
- ALB/NLB provisioning request schema
- managed RDS/ElastiCache failover policy
- cross-region data replication contract
- concrete EKS manifest mutation loop

## 공개 검증

- unit contract: `WorldsAwsContractTest.*`
- dedicated provider-path smoke: `CorePublicApiWorldsAwsSmoke`
- public-api smoke: `core_public_api_smoke`
- stable-header scenarios: `core_public_api_stable_header_scenarios`
- installed consumer: `CoreInstalledPackageConsumer`
