# Full-stack Docker runtime + observability (Prometheus/Grafana) wrapper.
#
# Standard runtime for the server stack is Linux(Docker). This script brings up
# the full stack with the "observability" profile enabled.

param(
  [switch]$NoBuild,
  [switch]$NoCache,
  [switch]$NoBase,
  [string]$ProjectName = ""
)

$ErrorActionPreference = 'Stop'
try { chcp 65001 | Out-Null } catch {}

$deploy = Join-Path $PSScriptRoot 'deploy_docker.ps1'
if (-not (Test-Path $deploy)) {
  Write-Error "Missing script: $deploy"
}

$build = -not $NoBuild

& $deploy -Action up -Detached -Build:$build -Observability -NoCache:$NoCache -NoBase:$NoBase -ProjectName $ProjectName
exit $LASTEXITCODE
