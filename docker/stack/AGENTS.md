# Local Docker chat runtime

Use scripts/deploy_docker.ps1. Services: postgres, redis, migrator, server-1/server-2, gateway-1/gateway-2, wb_worker, haproxy. Optional observability: prometheus, grafana, redis_exporter, postgres_exporter.

Startup server manifests: topologies/default.json and scripts/generate_stack_topology.py. Generated compose and active JSON are ignored. Manifest fields are instance_id, shard, TCP/metrics host ports, optional extra_tags; no dynamic orchestration.

Default TCP ingress: 6000. Server metrics: 39091/39092, gateway: 36001/36002, worker: 39093. Development auth is explicit; see docs/ops/authenticated-chat.md for signed mode. Keep data volumes on ordinary down.
