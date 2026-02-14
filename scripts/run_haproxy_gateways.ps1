# Deprecated wrapper.
#
# This script used to run HAProxy on Windows and spawn multiple gateway_app
# instances on the host. The project now standardizes on Linux runtime for the
# server stack (Docker), so HAProxy should run as a container.
#
# Use instead:
#   scripts/deploy_docker.ps1 -Action up -Detached -Build

param(
  # Keep legacy parameters so old invocations don't break.
  [string]$Config = 'Debug',
  [string]$BuildDir = 'build-windows',
  [int]$GatewayCount = 2,
  [int]$HaproxyPort = 6000,
  [int]$GatewayBasePort = 6101,
  [int]$MetricsBasePort = 6201,
  [string]$GatewayIdPrefix = 'gateway-local',
  [string]$HaproxyPath = 'haproxy',
  [string]$GatewayExe = '',
  [switch]$NoDotEnv,
  [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
try { chcp 65001 | Out-Null } catch {}

Write-Host "[warn] scripts/run_haproxy_gateways.ps1 is deprecated." -ForegroundColor Yellow
Write-Host "[warn] HAProxy is expected to run on Linux (Docker), not Windows host." -ForegroundColor Yellow
Write-Host "[warn] Use: scripts/deploy_docker.ps1 -Action up -Detached -Build" -ForegroundColor Yellow

$deploy = Join-Path $PSScriptRoot 'deploy_docker.ps1'
if (-not (Test-Path $deploy)) {
  Write-Error "Missing script: $deploy"
}

$build = -not $NoBuild
& $deploy -Action up -Detached -Build:$build
exit $LASTEXITCODE
