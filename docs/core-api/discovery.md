# Discovery API Guide

## Stability

| Header | Stability |
|---|---|
| `server/core/discovery/instance_registry.hpp` | `[Stable]` |
| `server/core/discovery/world_lifecycle_policy.hpp` | `[Stable]` |

## Canonical Naming
- canonical include path family: `server/core/discovery/**`
- canonical namespace: `server::core::discovery`
- `server/core/state/**` remains the underlying implementation/integration path and is not the consumer-facing stable surface

## Scope
- This surface exposes shared instance-discovery records, selector logic, backend interface, and in-memory backend behavior.
- It also exposes the minimal world lifecycle policy serialization/parsing contract used across gateway/control-plane/runtime state handoff.
- It does not promote sticky session-directory ownership.
- It does not promote concrete Redis or Consul discovery adapters.

## Public Contract
- `InstanceRecord` is the shared instance-discovery snapshot used for routing and control-plane inventory.
- `InstanceSelector`, `SelectorPolicyLayer`, `SelectorMatchStats`, `matches_selector()`, `classify_selector_policy_layer()`, `selector_policy_layer_name()`, and `select_instances()` define the stable selector/filter surface.
- `IInstanceStateBackend` is the stable backend interface boundary for discovery stores.
- `InMemoryStateBackend` is a stable local/test backend for single-process or consumer-side proofs.
- `WorldLifecyclePolicy`, `parse_world_lifecycle_policy()`, and `serialize_world_lifecycle_policy()` define the shared drain/replacement owner policy document contract.

## Non-Goals
- no Redis-backed registry construction contract
- no Consul adapter contract
- no sticky `SessionDirectory` contract
- no gateway-specific least-connections or sticky-routing policy behavior

## Public Proof
- public-api smoke/header scenarios include the discovery headers directly
- installed consumer proof: `CoreInstalledPackageConsumer`
- focused discovery behavior/unit proof:
  - `tests/state_instance_registry_tests.cpp`
  - `tests/gateway_session_directory_tests.cpp`
