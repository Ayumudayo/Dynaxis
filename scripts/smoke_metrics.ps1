<#
  Service-level /metrics smoke check for Docker stack.

  Verifies that gateway/server/wb_worker metric endpoints:
  - respond with HTTP 200
  - contain build info
  - contain common core runtime metrics
  - contain one service-specific metric
#>

[CmdletBinding()]
param(
  [string[]]$ServerMetricsUrls = @(
    "http://127.0.0.1:39091/metrics",
    "http://127.0.0.1:39092/metrics"
  ),
  [string[]]$GatewayMetricsUrls = @(
    "http://127.0.0.1:36001/metrics",
    "http://127.0.0.1:36002/metrics"
  ),
  [string[]]$WorkerMetricsUrls = @(
    "http://127.0.0.1:39093/metrics"
  ),
  [int]$TimeoutSec = 10
)

$ErrorActionPreference = 'Stop'

function Info([string]$Message) { Write-Host "[info] $Message" -ForegroundColor Cyan }
function Fail([string]$Message) { Write-Host "[fail] $Message" -ForegroundColor Red; $script:hadFailure = $true }

$hadFailure = $false

function Assert-MetricsEndpoint {
  param(
    [Parameter(Mandatory = $true)] [string]$Url,
    [Parameter(Mandatory = $true)] [string[]]$RequiredMarkers
  )

  Info "Checking $Url"
  try {
    $resp = Invoke-WebRequest -Method Get -Uri $Url -TimeoutSec $TimeoutSec
  }
  catch {
    Fail "Request failed: $Url ($($_.Exception.Message))"
    return
  }

  if ($resp.StatusCode -ne 200) {
    Fail "Unexpected status code $($resp.StatusCode): $Url"
    return
  }

  $body = [string]$resp.Content
  if ([string]::IsNullOrWhiteSpace($body)) {
    Fail "Empty /metrics body: $Url"
    return
  }

  foreach ($marker in $RequiredMarkers) {
    if ($body.Contains($marker)) {
      continue
    }
    Fail "Missing metric marker '$marker' in $Url"
  }
}

$commonMarkers = @(
  "knights_build_info",
  "core_runtime_session_started_total"
)

foreach ($url in $ServerMetricsUrls) {
  Assert-MetricsEndpoint -Url $url -RequiredMarkers ($commonMarkers + @("chat_session_active"))
}

foreach ($url in $GatewayMetricsUrls) {
  Assert-MetricsEndpoint -Url $url -RequiredMarkers ($commonMarkers + @("gateway_sessions_active"))
}

foreach ($url in $WorkerMetricsUrls) {
  Assert-MetricsEndpoint -Url $url -RequiredMarkers ($commonMarkers + @("wb_pending"))
}

if ($hadFailure) {
  exit 2
}

Info "Metrics smoke check passed."
