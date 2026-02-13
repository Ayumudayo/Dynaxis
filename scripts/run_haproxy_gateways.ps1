# Local dev helper: run HAProxy + multiple gateway_app instances.
param(
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

function Info($m){ Write-Host "[info] $m" -ForegroundColor Cyan }
function Warn($m){ Write-Host "[warn] $m" -ForegroundColor Yellow }
function Fail($m){ Write-Host "[fail] $m" -ForegroundColor Red; exit 1 }

function Load-DotEnv([string]$Path) {
  if (-not (Test-Path $Path)) { return }
  Info "Loading env file: $Path"
  foreach ($raw in Get-Content -Path $Path) {
    $line = $raw.Trim()
    if (-not $line) { continue }
    if ($line.StartsWith('#')) { continue }
    $eq = $line.IndexOf('=')
    if ($eq -lt 1) { continue }
    $key = $line.Substring(0, $eq).Trim()
    $val = $line.Substring($eq + 1).Trim()
    if ($val.Length -ge 2 -and (($val[0] -eq '"' -and $val[$val.Length - 1] -eq '"') -or ($val[0] -eq "'" -and $val[$val.Length - 1] -eq "'"))) {
      $val = $val.Substring(1, $val.Length - 2)
    }
    if ($key) { $env:$key = $val }
  }
}

if (-not $NoDotEnv) {
  Load-DotEnv (Join-Path (Get-Location) '.env')
}

if (-not $NoBuild) {
  Info "Build gateway_app ($Config, $BuildDir)"
  & ./scripts/build.ps1 -Config $Config -BuildDir $BuildDir -Target gateway_app | Out-Null
  if ($LASTEXITCODE -ne 0) { Fail "빌드 실패: gateway_app" }
}

if (-not $GatewayExe -or $GatewayExe -eq '') {
  $GatewayExe = Join-Path $BuildDir "gateway/$Config/gateway_app.exe"
  if (-not (Test-Path $GatewayExe)) {
    $GatewayExe = Join-Path $BuildDir 'gateway_app.exe'
  }
}
if (-not (Test-Path $GatewayExe)) {
  Fail "gateway_app 실행 파일을 찾을 수 없습니다: $GatewayExe"
}

if ($GatewayCount -lt 1) { Fail "GatewayCount must be >= 1" }

$hap = Get-Command $HaproxyPath -ErrorAction SilentlyContinue
if (-not $hap) {
  Fail "haproxy를 찾을 수 없습니다: $HaproxyPath (PATH에 설치하거나 -HaproxyPath 지정)"
}

$gatewayProcs = @()
$haproxyProc = $null
$cfgPath = $null

try {
  Info "Starting $GatewayCount gateway_app instances"
  for ($i = 0; $i -lt $GatewayCount; $i++) {
    $listenPort = $GatewayBasePort + $i
    $metricsPort = $MetricsBasePort + $i
    $gatewayId = "$GatewayIdPrefix-$listenPort"

    $cmd = @(
      "`$env:GATEWAY_LISTEN='127.0.0.1:$listenPort'",
      "`$env:METRICS_PORT='$metricsPort'",
      "`$env:GATEWAY_ID='$gatewayId'",
      "& '$GatewayExe'"
    ) -join '; '

    $p = Start-Process -FilePath 'powershell' -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-Command', $cmd) -PassThru
    Start-Sleep -Milliseconds 300
    if ($p.HasExited) { Fail "gateway_app 즉시 종료(listen=$listenPort, code=$($p.ExitCode))" }
    $gatewayProcs += $p
    Info "gateway pid=$($p.Id) listen=127.0.0.1:$listenPort metrics=:$metricsPort id=$gatewayId"
  }

  $cfg = @()
  $cfg += "global"
  $cfg += "  maxconn 10000"
  $cfg += ""
  $cfg += "defaults"
  $cfg += "  mode tcp"
  $cfg += "  timeout connect 3s"
  $cfg += "  timeout client  60s"
  $cfg += "  timeout server  60s"
  $cfg += ""
  $cfg += "frontend fe_gateway"
  $cfg += "  bind 0.0.0.0:$HaproxyPort"
  $cfg += "  default_backend be_gateway"
  $cfg += ""
  $cfg += "backend be_gateway"
  $cfg += "  balance roundrobin"
  for ($i = 0; $i -lt $GatewayCount; $i++) {
    $listenPort = $GatewayBasePort + $i
    $cfg += "  server gw$($i+1) 127.0.0.1:$listenPort check"
  }

  $tmp = [System.IO.Path]::GetTempPath()
  $cfgPath = Join-Path $tmp ("knights-haproxy-{0}.cfg" -f ([System.Guid]::NewGuid().ToString('N')))
  Set-Content -Path $cfgPath -Value ($cfg -join "`n") -Encoding ascii
  Info "HAProxy config: $cfgPath"

  Info "Starting HAProxy on :$HaproxyPort"
  $haproxyProc = Start-Process -FilePath $HaproxyPath -ArgumentList @('-f', $cfgPath, '-db') -PassThru
  Start-Sleep -Milliseconds 300
  if ($haproxyProc.HasExited) { Fail "haproxy 즉시 종료(code=$($haproxyProc.ExitCode))" }
  Info "haproxy pid=$($haproxyProc.Id)"

  Info "Ready: Client -> 127.0.0.1:$HaproxyPort -> gateways (count=$GatewayCount)"
  Info "Stop: Ctrl+C 또는 아무 키나 누르세요..."
  try { [void][System.Console]::ReadKey($true) } catch {}
}
finally {
  Info "Stopping processes..."
  if ($haproxyProc -and -not $haproxyProc.HasExited) {
    Stop-Process -Id $haproxyProc.Id -Force -ErrorAction SilentlyContinue
  }
  foreach ($p in $gatewayProcs) {
    if ($p -and -not $p.HasExited) {
      Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    }
  }
  if ($cfgPath -and (Test-Path $cfgPath)) {
    Remove-Item -Force $cfgPath -ErrorAction SilentlyContinue
  }
  Info "Done"
}

exit 0
