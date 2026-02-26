<#
  Quick observability sanity check for the local Docker stack.

  - Verifies Prometheus is reachable and expected jobs have UP targets.
  - Optionally verifies Grafana health endpoint.

  This is a lightweight operator check; it is not meant to be exhaustive.
 #>

[CmdletBinding()]
param(
  [string]$PrometheusUrl = "http://127.0.0.1:39090",
  [string]$GrafanaUrl = "http://127.0.0.1:33000",
  [string[]]$ExpectedJobs = @("chat_server", "gateway", "write_behind", "admin_app", "haproxy", "redis", "postgres"),
  [switch]$SkipGrafana
)

$ErrorActionPreference = 'Stop'

function Info([string]$Message) { Write-Host "[info] $Message" -ForegroundColor Cyan }
function Warn([string]$Message) { Write-Host "[warn] $Message" -ForegroundColor Yellow }
function Fail([string]$Message) { Write-Host "[fail] $Message" -ForegroundColor Red; $script:hadFailure = $true }

$hadFailure = $false

Info "Checking Prometheus targets: $PrometheusUrl"

try {
  $targets = Invoke-RestMethod -Method Get -Uri "$PrometheusUrl/api/v1/targets" -TimeoutSec 10
} catch {
  Fail "Prometheus unreachable: $($_.Exception.Message)"
  exit 1
}

if (-not $targets -or $targets.status -ne 'success') {
  Fail "Prometheus /api/v1/targets returned unexpected response."
  exit 1
}

$active = @()
try { $active = $targets.data.activeTargets } catch { $active = @() }

foreach ($job in $ExpectedJobs) {
  $jobTargets = @($active | Where-Object { $_.labels.job -eq $job })
  if (-not $jobTargets -or $jobTargets.Count -eq 0) {
    Fail "Missing job in targets: $job"
    continue
  }

  $up = @($jobTargets | Where-Object { $_.health -eq 'up' })
  $down = @($jobTargets | Where-Object { $_.health -ne 'up' })

  if ($down.Count -gt 0) {
    Fail "Job '$job' has DOWN targets: up=$($up.Count) down=$($down.Count)"
  } else {
    Info "Job '$job' OK: $($up.Count) target(s) up"
  }
}

if (-not $SkipGrafana) {
  Info "Checking Grafana health: $GrafanaUrl"
  try {
    $health = Invoke-RestMethod -Method Get -Uri "$GrafanaUrl/api/health" -TimeoutSec 10
    if ($health -and $health.database -and $health.database -ne 'ok') {
      Warn "Grafana health check returned database=$($health.database)"
    } else {
      Info "Grafana OK"
    }
  } catch {
    Warn "Grafana health endpoint failed: $($_.Exception.Message)"
  }
}

if ($hadFailure) {
  exit 2
}

Info "Observability looks healthy."
