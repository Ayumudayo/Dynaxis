# Local full stack runner: (optional) Docker infra + server_app cluster + gateway_app cluster + HAProxy.
param(
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
    if ($key) { Set-Item -Path "Env:$key" -Value $val }
  }
}

function Push-Env([hashtable]$Overrides) {
  $saved = @{}
  foreach ($k in $Overrides.Keys) {
    $saved[$k] = [System.Environment]::GetEnvironmentVariable($k, 'Process')
    $v = $Overrides[$k]
    if ($null -eq $v) {
      Remove-Item -Path "Env:$k" -ErrorAction SilentlyContinue
    } else {
      Set-Item -Path "Env:$k" -Value ([string]$v)
    }
  }
  return $saved
}

function Pop-Env([hashtable]$Saved) {
  foreach ($k in $Saved.Keys) {
    $v = $Saved[$k]
    if ($null -eq $v) {
      Remove-Item -Path "Env:$k" -ErrorAction SilentlyContinue
    } else {
      Set-Item -Path "Env:$k" -Value $v
    }
  }
}

function Resolve-Exe([string]$Name) {
  $ext = '.exe'
  $candidates = @(
    (Join-Path $BuildDir "$Name$ext"),
    (Join-Path $BuildDir (Join-Path $Config "$Name$ext")),
    (Join-Path $BuildDir (Join-Path 'server' (Join-Path $Config "$Name$ext"))),
    (Join-Path $BuildDir (Join-Path 'gateway' (Join-Path $Config "$Name$ext"))),
    (Join-Path $BuildDir (Join-Path 'tools' (Join-Path $Config "$Name$ext")))
  )
  foreach ($c in $candidates) {
    if ($c -and (Test-Path $c)) { return (Resolve-Path $c).Path }
  }
  try {
    $found = Get-ChildItem -Path $BuildDir -Recurse -File -Filter "$Name$ext" -ErrorAction SilentlyContinue |
             Sort-Object LastWriteTime -Descending |
             Select-Object -First 1
    if ($found) { return $found.FullName }
  } catch {}
  return $null
}

if ($ServerCount -lt 1) { Fail "ServerCount must be >= 1" }
if ($GatewayCount -lt 1) { Fail "GatewayCount must be >= 1" }

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $repoRoot

if (-not $NoDotEnv) {
  Load-DotEnv (Join-Path $repoRoot '.env')
}

if (-not $RedisUri -or $RedisUri -eq '') {
  if ($WithDockerInfra) { $RedisUri = 'tcp://127.0.0.1:36379' }
  else { $RedisUri = 'tcp://127.0.0.1:6379' }
}

if (-not $DbUri -or $DbUri -eq '') {
  if ($WithPostgres) {
    if ($WithDockerInfra) {
      $DbUri = 'postgresql://knights:password@127.0.0.1:35432/knights_db'
    } else {
      Fail "DbUri must be provided when -WithPostgres is used without -WithDockerInfra"
    }
  }
}

if (-not $RegistryPrefix.EndsWith('/')) { $RegistryPrefix += '/' }

$infraCompose = Join-Path $repoRoot 'docker/infra/docker-compose.yml'

$serverProcs = @()
$gatewayProcs = @()
$workerProc = $null
$haproxyProc = $null
$haproxyCfgPath = $null

try {
  if ($WithDockerInfra) {
    if (-not (Test-Path $infraCompose)) { Fail "Missing infra compose file: $infraCompose" }
    try { docker --version | Out-Null; docker compose version | Out-Null } catch { Fail "docker or docker compose not found in PATH" }

    $services = @('redis')
    if ($WithPostgres) { $services += 'postgres' }
    Info ("Starting docker infra: " + ($services -join ', '))
    & docker compose -f $infraCompose up -d @services | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail "docker compose up failed" }
    Start-Sleep -Seconds 1
  }

  if (-not $NoBuild) {
    $targets = @('server_app','gateway_app')
    if ($RunMigrations) { $targets += 'migrations_runner' }
    if ($WithWorker) { $targets += 'wb_worker' }
    $targets = $targets | Select-Object -Unique
    Info ("Build targets: " + ($targets -join ', '))
    foreach ($t in $targets) {
      & ./scripts/build.ps1 -Config $Config -BuildDir $BuildDir -Target $t | Out-Null
      if ($LASTEXITCODE -ne 0) { Fail "빌드 실패: $t" }
    }
  }

  $serverExe = Resolve-Exe 'server_app'
  $gatewayExe = Resolve-Exe 'gateway_app'
  if (-not $serverExe) { Fail "server_app executable not found under $BuildDir" }
  if (-not $gatewayExe) { Fail "gateway_app executable not found under $BuildDir" }

  if ($RunMigrations) {
    if (-not $WithPostgres) { Fail "-RunMigrations requires -WithPostgres" }
    if (-not $DbUri -or $DbUri -eq '') { Fail "DB_URI is required to run migrations" }
    $migExe = Resolve-Exe 'migrations_runner'
    if (-not $migExe) { Fail "migrations_runner executable not found under $BuildDir" }
    Info "Running migrations_runner"
    & $migExe --db-uri $DbUri
    if ($LASTEXITCODE -ne 0) { Fail "migrations_runner failed" }
  }

  if ($WithWorker) {
    if (-not $WithPostgres) { Fail "-WithWorker requires -WithPostgres" }
    $workerExe = Resolve-Exe 'wb_worker'
    if (-not $workerExe) { Fail "wb_worker executable not found under $BuildDir" }
  }

  Info "Starting $ServerCount server_app instances"
  for ($i = 0; $i -lt $ServerCount; $i++) {
    $port = $ServerBasePort + $i
    $instanceId = "$ServerIdPrefix-$port"

    $dbValue = ''
    if ($DbUri -and $DbUri -ne '') { $dbValue = $DbUri }

    $overrides = @{
      'PORT' = "$port";
      'DB_URI' = $dbValue;
      'REDIS_URI' = $RedisUri;
      'METRICS_PORT' = '';
      'SERVER_ADVERTISE_HOST' = $ServerAdvertiseHost;
      'SERVER_ADVERTISE_PORT' = "$port";
      'SERVER_INSTANCE_ID' = $instanceId;
      'SERVER_REGISTRY_PREFIX' = $RegistryPrefix;
      'SERVER_REGISTRY_TTL' = "$RegistryTtl";
      'SERVER_HEARTBEAT_INTERVAL' = "$HeartbeatInterval";
    }

    $saved = Push-Env $overrides
    try {
      $p = Start-Process -FilePath $serverExe -ArgumentList @("$port") -PassThru
      Start-Sleep -Milliseconds 500
      if ($p.HasExited) { Fail "server_app exited early (port=$port, code=$($p.ExitCode))" }
      $serverProcs += $p
      Info "server pid=$($p.Id) port=$port instance=$instanceId"
    } finally {
      Pop-Env $saved
    }
  }

  Info "Starting $GatewayCount gateway_app instances"
  for ($i = 0; $i -lt $GatewayCount; $i++) {
    $listenPort = $GatewayBasePort + $i
    $metricsPort = $GatewayMetricsBasePort + $i
    $gatewayId = "$GatewayIdPrefix-$listenPort"

    $overrides = @{
      'GATEWAY_LISTEN' = "127.0.0.1:$listenPort";
      'METRICS_PORT' = "$metricsPort";
      'GATEWAY_ID' = $gatewayId;
      'REDIS_URI' = $RedisUri;
      'SERVER_REGISTRY_PREFIX' = $RegistryPrefix;
      'SERVER_REGISTRY_TTL' = "$RegistryTtl";
      'ALLOW_ANONYMOUS' = '1';
    }

    $saved = Push-Env $overrides
    try {
      $p = Start-Process -FilePath $gatewayExe -PassThru
      Start-Sleep -Milliseconds 500
      if ($p.HasExited) { Fail "gateway_app exited early (listen=$listenPort, code=$($p.ExitCode))" }
      $gatewayProcs += $p
      Info "gateway pid=$($p.Id) listen=127.0.0.1:$listenPort metrics=:$metricsPort id=$gatewayId"
    } finally {
      Pop-Env $saved
    }
  }

  $hap = Get-Command $HaproxyPath -ErrorAction SilentlyContinue
  if (-not $hap) { Fail "haproxy not found: $HaproxyPath" }

  $cfg = @()
  $cfg += 'global'
  $cfg += '  maxconn 10000'
  $cfg += ''
  $cfg += 'defaults'
  $cfg += '  mode tcp'
  $cfg += '  timeout connect 3s'
  $cfg += '  timeout client  60s'
  $cfg += '  timeout server  60s'
  $cfg += ''
  $cfg += 'frontend fe_gateway'
  $cfg += "  bind 0.0.0.0:$HaproxyPort"
  $cfg += '  default_backend be_gateway'
  $cfg += ''
  $cfg += 'backend be_gateway'
  $cfg += '  balance roundrobin'
  for ($i = 0; $i -lt $GatewayCount; $i++) {
    $listenPort = $GatewayBasePort + $i
    $cfg += "  server gw$($i+1) 127.0.0.1:$listenPort check"
  }
  $tmp = [System.IO.Path]::GetTempPath()
  $haproxyCfgPath = Join-Path $tmp ("knights-haproxy-{0}.cfg" -f ([System.Guid]::NewGuid().ToString('N')))
  Set-Content -Path $haproxyCfgPath -Value ($cfg -join "`n") -Encoding ascii
  Info "HAProxy config: $haproxyCfgPath"

  Info "Starting HAProxy on :$HaproxyPort"
  $haproxyProc = Start-Process -FilePath $HaproxyPath -ArgumentList @('-f', $haproxyCfgPath, '-db') -PassThru
  Start-Sleep -Milliseconds 500
  if ($haproxyProc.HasExited) { Fail "haproxy exited early (code=$($haproxyProc.ExitCode))" }
  Info "haproxy pid=$($haproxyProc.Id)"

  if ($WithWorker) {
    $workerExe = Resolve-Exe 'wb_worker'
    $overrides = @{
      'DB_URI' = $DbUri;
      'REDIS_URI' = $RedisUri;
    }
    $saved = Push-Env $overrides
    try {
      Info "Starting wb_worker"
      $workerProc = Start-Process -FilePath $workerExe -PassThru
      Start-Sleep -Milliseconds 500
      if ($workerProc.HasExited) { Fail "wb_worker exited early (code=$($workerProc.ExitCode))" }
      Info "wb_worker pid=$($workerProc.Id)"
    } finally {
      Pop-Env $saved
    }
  }

  Info "Ready: Client -> 127.0.0.1:$HaproxyPort -> gateways ($GatewayCount) -> servers ($ServerCount)"
  Info "Stop: Ctrl+C 또는 아무 키나 누르세요..."
  try { [void][System.Console]::ReadKey($true) } catch {}
}
finally {
  Info "Stopping processes..."
  if ($haproxyProc -and -not $haproxyProc.HasExited) { Stop-Process -Id $haproxyProc.Id -Force -ErrorAction SilentlyContinue }
  if ($workerProc -and -not $workerProc.HasExited) { Stop-Process -Id $workerProc.Id -Force -ErrorAction SilentlyContinue }
  foreach ($p in $gatewayProcs) { if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } }
  foreach ($p in $serverProcs) { if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } }
  if ($haproxyCfgPath -and (Test-Path $haproxyCfgPath)) { Remove-Item -Force $haproxyCfgPath -ErrorAction SilentlyContinue }

  if ($WithDockerInfra -and $StopInfraOnExit) {
    try {
      Info "Stopping docker infra"
      & docker compose -f $infraCompose down | Out-Null
    } catch {}
  }
  Info "Done"
}

exit 0
