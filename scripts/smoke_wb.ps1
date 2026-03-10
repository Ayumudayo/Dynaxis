# Write-behind 통합 스모크 테스트 (PowerShell)
param(
  [string]$Config = 'Debug',
  [string]$BuildDir = 'build-windows'
)

$ErrorActionPreference = 'Stop'
try { chcp 65001 | Out-Null } catch {}

function Info($m){ Write-Host "[info] $m" -ForegroundColor Cyan }
function Fail($m){ Write-Host "[fail] $m" -ForegroundColor Red; exit 1 }

# 1) 환경 확인
if (-not (Test-Path '.env')) { Write-Host "[warn] .env 없음 — OS 환경변수 사용" -ForegroundColor Yellow }
if (-not $env:DB_URI) { Write-Host "[warn] DB_URI 미설정 — .env에서 로드되길 기대" -ForegroundColor Yellow }
if (-not $env:REDIS_URI) { Write-Host "[warn] REDIS_URI 미설정 — .env에서 로드되길 기대" -ForegroundColor Yellow }

# 2) 빌드(필요 타깃)
Info "필요 타깃 빌드: wb_worker, wb_emit, wb_check"
./scripts/build.ps1 -Config $Config -BuildDir $BuildDir -Target wb_worker | Out-Null
./scripts/build.ps1 -Config $Config -BuildDir $BuildDir -Target wb_emit | Out-Null
./scripts/build.ps1 -Config $Config -BuildDir $BuildDir -Target wb_check | Out-Null

$wbWorker = if (Test-Path (Join-Path $BuildDir "$Config/wb_worker.exe")) { Join-Path $BuildDir "$Config/wb_worker.exe" } else { Join-Path $BuildDir 'wb_worker' }
$wbEmit   = if (Test-Path (Join-Path $BuildDir "$Config/wb_emit.exe"))   { Join-Path $BuildDir "$Config/wb_emit.exe" }   else { Join-Path $BuildDir 'wb_emit' }
$wbCheck  = if (Test-Path (Join-Path $BuildDir "$Config/wb_check.exe"))  { Join-Path $BuildDir "$Config/wb_check.exe" }  else { Join-Path $BuildDir 'wb_check' }
if (-not (Test-Path $wbWorker)) { Fail "wb_worker 실행 파일을 찾을 수 없습니다: $wbWorker" }
if (-not (Test-Path $wbEmit))   { Fail "wb_emit 실행 파일을 찾을 수 없습니다: $wbEmit" }
if (-not (Test-Path $wbCheck))  { Fail "wb_check 실행 파일을 찾을 수 없습니다: $wbCheck" }

# 3) wb_worker 백그라운드 실행
Info "wb_worker 백그라운드 시작"
$p = Start-Process -FilePath $wbWorker -PassThru
Start-Sleep -Milliseconds 500

try {
  # 4) 스트림에 이벤트 적재
  Info "이벤트 XADD(wb_emit)"
  $eventOutput = & $wbEmit 'session_login'
  if ($LASTEXITCODE -ne 0 -or -not $eventOutput) { Fail "wb_emit 실패" }
  $eventOutput = ($eventOutput | Out-String).Trim()
  $eventId = if ($eventOutput -match 'ID:\s*([^\s]+)$') { $Matches[1] } else { $eventOutput }
  Info "event_id=$eventId"

  # 5) 커밋 대기 후 확인
  Start-Sleep -Seconds 1
  & $wbCheck $eventId
  if ($LASTEXITCODE -ne 0) { Fail "DB 커밋 확인 실패(event_id=$eventId)" }
  Info "스모크 성공: event_id=$eventId 반영됨"
}
finally {
  if ($p -and -not $p.HasExited) {
    Info "wb_worker 종료"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
  }
}

exit 0
