# MMORPG Scaling Orchestration Charter

This document defines the future orchestration tranche for live scaling semantics on the MMORPG branch.

It is a design/charter document only. No live scaling implementation is introduced by the current slice.

## Purpose

- describe how live scaling work should be framed when it eventually starts
- keep the current startup-only topology contract bounded
- avoid collapsing desired topology, world lifecycle policy, and concrete instance orchestration into one contract

## Orchestrator-Agnostic Principle

The future scaling model is orchestrator-agnostic.

This repo may define:

- desired topology contract
- observed topology/read-model visibility
- policy visibility
- proof and validation tooling

This repo should not assume one concrete runtime orchestrator as part of the contract:

- not Docker-only
- not Kubernetes-only

Concrete instance create/remove actions are the responsibility of an external orchestrator.

## Role Split

### Repo / control-plane responsibilities

- define the desired topology contract
- expose observed topology and policy/read-model visibility
- validate desired topology documents
- prove routing/resume behavior against observed topology

### External orchestrator responsibilities

- create instances
- remove instances
- place instances
- restart or replace unhealthy instances
- perform rollout / rollback mechanics

## Reconciliation Model

The future orchestration tranche should use explicit reconciliation phases:

- `pending`
- `reconciling`
- `stable`
- `degraded`

Phase intent:

- `pending`
  - a new desired revision exists but application has not begun
- `reconciling`
  - desired and observed topology are converging
- `stable`
  - observed topology satisfies the desired contract
- `degraded`
  - convergence stalled or the observed topology cannot currently satisfy desired state

These phases are design commitments only for now.

No runtime code emits them yet.

## Inputs And Outputs

Future orchestration input:

- pool-based desired topology contract

Future orchestration observed state:

- concrete instances
- readiness / health
- placement result
- world inventory / owner visibility

Future orchestration must not treat world lifecycle policy as the same object as desired topology.

## Entry Criteria For A Future Implementation Tranche

Do not start live scaling implementation until all of the following are true:

- startup-only topology manifests are considered stable
- same-world replacement proof remains green
- a concrete need for live scale semantics exists
- the external orchestrator boundary is chosen for the target environment

## Explicit Non-Goals

- no scheduler implementation in this tranche
- no active owner transfer choreography in this tranche
- no admin endpoint for scaling in this tranche
- no Redis desired-topology key in this tranche
- no gameplay/world migration work in this tranche
