# 원클릭 실행 스크립트: 서버 + 워커(+옵션 DLQ/클라이언트) 기동 및 스모크
param(
  [string]$Config = 'Debug',
  [string]$BuildDir = 'build-msvc',
  [int]$Port = 5000,
  [switch]$RunDLQ,
  [switch]$WithClient,
  [switch]$Smoke,
  [switch]$UseVcpkg
)

$ErrorActionPreference = 'Stop'
try { chcp 65001 | Out-Null } catch {}

function Info($m){ Write-Host "[info] $m" -ForegroundColor Cyan }
function Warn($m){ Write-Host "[warn] $m" -ForegroundColor Yellow }
function Fail($m){ Write-Host "[fail] $m" -ForegroundColor Red; exit 1 }

if (-not (Test-Path '.env')) { Warn ".env 없음 — OS 환경변수 사용" }

# 1) 빌드
$targets = @('server_app','wb_worker')
if ($RunDLQ)     { $targets += 'wb_dlq_replayer' }
if ($WithClient) { $targets += 'dev_chat_cli' }
Info ("필요 타깃 빌드: " + ($targets -join ', '))
$argsCommon = @{ Config = $Config; BuildDir = $BuildDir }
if ($UseVcpkg) { $argsCommon['UseVcpkg'] = $true }
& ./scripts/build.ps1 @argsCommon -Target server_app | Out-Null
if ($LASTEXITCODE -ne 0) { Fail "빌드 실패: server_app" }
& ./scripts/build.ps1 @argsCommon -Target wb_worker   | Out-Null
if ($LASTEXITCODE -ne 0) { Fail "빌드 실패: wb_worker" }
if ($RunDLQ)    { & ./scripts/build.ps1 @argsCommon -Target wb_dlq_replayer | Out-Null; if ($LASTEXITCODE -ne 0) { Fail "빌드 실패: wb_dlq_replayer" } }
if ($WithClient){ & ./scripts/build.ps1 @argsCommon -Target dev_chat_cli    | Out-Null; if ($LASTEXITCODE -ne 0) { Fail "빌드 실패: dev_chat_cli" } }

# 2) 실행 파일 경로 계산
$serverExe = if (Test-Path (Join-Path $BuildDir "server/$Config/server_app.exe")) { Join-Path $BuildDir "server/$Config/server_app.exe" } else { Join-Path $BuildDir 'server_app' }
$workerExe = if (Test-Path (Join-Path $BuildDir "$Config/wb_worker.exe"))        { Join-Path $BuildDir "$Config/wb_worker.exe" }        else { Join-Path $BuildDir 'wb_worker' }
$dlqExe    = if (Test-Path (Join-Path $BuildDir "$Config/wb_dlq_replayer.exe"))  { Join-Path $BuildDir "$Config/wb_dlq_replayer.exe" }  else { Join-Path $BuildDir 'wb_dlq_replayer' }
$cliExe    = if (Test-Path (Join-Path $BuildDir "devclient/$Config/dev_chat_cli.exe")) { Join-Path $BuildDir "devclient/$Config/dev_chat_cli.exe" } else { Join-Path $BuildDir 'dev_chat_cli' }
if (-not (Test-Path $serverExe)) { Fail "server_app 실행 파일을 찾을 수 없습니다: $serverExe" }
if (-not (Test-Path $workerExe)) { Fail "wb_worker 실행 파일을 찾을 수 없습니다: $workerExe" }

# 3) 백그라운드 기동
Info "server_app 시작 :$Port"
$serverP = Start-Process -FilePath $serverExe -ArgumentList $Port -PassThru
Start-Sleep -Milliseconds 800
if ($serverP.HasExited) { Fail "server_app 비정상 종료(code=$($serverP.ExitCode))" }
Info "server pid=$($serverP.Id)"

Info "wb_worker 시작"
$workerP = Start-Process -FilePath $workerExe -PassThru
Start-Sleep -Milliseconds 800
if ($workerP.HasExited) { Fail "wb_worker 비정상 종료(code=$($workerP.ExitCode))" }
Info "worker pid=$($workerP.Id)"

if ($RunDLQ) {
  Info "wb_dlq_replayer 시작"
  if (-not (Test-Path $dlqExe)) { Fail "wb_dlq_replayer 실행 파일을 찾을 수 없습니다: $dlqExe" }
  $dlqP = Start-Process -FilePath $dlqExe -PassThru
  Start-Sleep -Milliseconds 500
  if ($dlqP.HasExited) { Warn "wb_dlq_replayer 즉시 종료(code=$($dlqP.ExitCode))" } else { Info "dlq pid=$($dlqP.Id)" }
}

if ($WithClient) {
  if (Test-Path $cliExe) {
    Info "개발 클라이언트(dev_chat_cli) 실행"
    Start-Process -FilePath $cliExe | Out-Null
  } else {
    Warn "dev_chat_cli 실행 파일을 찾을 수 없습니다: $cliExe"
  }
}

if ($Smoke) {
  Info "스모크: wb_emit → wb_check"
  & ./scripts/build.ps1 @argsCommon -Target wb_emit  | Out-Null
  if ($LASTEXITCODE -ne 0) { Warn "wb_emit 빌드 실패 — 스모크 생략"; $wbEmit = $null }
  & ./scripts/build.ps1 @argsCommon -Target wb_check | Out-Null
  if ($LASTEXITCODE -ne 0) { Warn "wb_check 빌드 실패 — 스모크 생략"; $wbCheck = $null }
  $wbEmit = if (Test-Path (Join-Path $BuildDir "$Config/wb_emit.exe")) { Join-Path $BuildDir "$Config/wb_emit.exe" } else { Join-Path $BuildDir 'wb_emit' }
  $wbCheck = if (Test-Path (Join-Path $BuildDir "$Config/wb_check.exe")) { Join-Path $BuildDir "$Config/wb_check.exe" } else { Join-Path $BuildDir 'wb_check' }
  if (-not (Test-Path $wbEmit))  { Warn "wb_emit 없음 — 스모크 생략" } else {
    $eid = & $wbEmit 'session_login'
    if ($LASTEXITCODE -ne 0 -or -not $eid) { Warn "wb_emit 실패" } else {
      $eid = $eid.Trim(); Info "event_id=$eid"
      Start-Sleep -Seconds 1
      if (Test-Path $wbCheck) { & $wbCheck $eid; if ($LASTEXITCODE -eq 0) { Info "스모크 성공" } else { Warn "스모크 확인 실패" } }
    }
  }
}

Info "종료: Ctrl+C 또는 아무 키나 누르세요..."
try { [void][System.Console]::ReadKey($true) } catch {}

Info "프로세스 종료 중..."
if ($workerP -and -not $workerP.HasExited) { Stop-Process -Id $workerP.Id -Force -ErrorAction SilentlyContinue }
if ($dlqP    -and -not $dlqP.HasExited)    { Stop-Process -Id $dlqP.Id    -Force -ErrorAction SilentlyContinue }
if ($serverP -and -not $serverP.HasExited) { Stop-Process -Id $serverP.Id -Force -ErrorAction SilentlyContinue }
Info "완료"

exit 0
