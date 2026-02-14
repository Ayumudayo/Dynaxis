# Deprecated wrapper.
#
# This repo's standard runtime is Linux (Docker). For a full-stack local run
# (HAProxy + gateways + servers + infra), use:
#   scripts/deploy_docker.ps1 -Action up -Detached -Build

param(
  # Keep legacy parameters so old invocations don't break.
  [string]$Config = 'Debug',
  [string]$BuildDir = 'build-windows',
  [switch]$NoBuild,
  [switch]$NoDotEnv,

  [switch]$WithDockerInfra,
  [switch]$WithPostgres,
  [switch]$RunMigrations,
  [switch]$WithWorker,
  [switch]$StopInfraOnExit,

  [int]$ServerCount = 2,
  [int]$ServerBasePort = 5101,
  [string]$ServerIdPrefix = 'server-local',
  [string]$ServerAdvertiseHost = '127.0.0.1',

  [int]$GatewayCount = 2,
  [int]$GatewayBasePort = 6101,
  [int]$GatewayMetricsBasePort = 6201,
  [string]$GatewayIdPrefix = 'gateway-local',

  [int]$HaproxyPort = 6000,
  [string]$HaproxyPath = 'haproxy',

  [string]$RedisUri = '',
  [string]$DbUri = '',
  [string]$RegistryPrefix = 'gateway/instances/',
  [int]$RegistryTtl = 30,
  [int]$HeartbeatInterval = 5
)

$ErrorActionPreference = 'Stop'
try { chcp 65001 | Out-Null } catch {}

Write-Host "[warn] scripts/run_full_stack.ps1 is deprecated." -ForegroundColor Yellow
Write-Host "[warn] Server stack runtime is Linux containers (Docker)." -ForegroundColor Yellow
Write-Host "[warn] Use: scripts/deploy_docker.ps1 -Action up -Detached -Build" -ForegroundColor Yellow

$deploy = Join-Path $PSScriptRoot 'deploy_docker.ps1'
if (-not (Test-Path $deploy)) {
  Write-Error "Missing script: $deploy"
}

$build = -not $NoBuild
& $deploy -Action up -Detached -Build:$build
exit $LASTEXITCODE
