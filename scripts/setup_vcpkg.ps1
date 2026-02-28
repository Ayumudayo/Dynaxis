<#
  Knights용 vcpkg 부트스트랩 + 의존성 설치 스크립트
  - external/vcpkg 아래에 공식 vcpkg 저장소를 자동으로 클론합니다.
  - OS에 맞는 bootstrap 스크립트를 호출해 vcpkg 실행파일을 생성합니다.
  - manifest(vcpkg.json)에 정의된 패키지를 지정된 triplet으로 설치합니다.
#>
[CmdletBinding()]
param(
  [string]$Triplet = "",
  [switch]$SkipInstall,
  [string]$Repo = "https://github.com/microsoft/vcpkg.git",
  [string[]]$Features = @()
)

$ErrorActionPreference = 'Stop'
function Info($msg){ Write-Host "[info] $msg" -ForegroundColor Cyan }
function Warn($msg){ Write-Host "[warn] $msg" -ForegroundColor Yellow }
function Fail($msg){ Write-Host "[fail] $msg" -ForegroundColor Red; exit 1 }

if (-not (Get-Variable -Name IsWindows -ErrorAction SilentlyContinue)) {
    $IsWindows = $env:OS -like '*Windows*'
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$vcpkgRoot = Join-Path $repoRoot "external/vcpkg"

function Get-VcpkgBaseline([string]$RepoRoot) {
  $vcpkgJson = Join-Path $RepoRoot 'vcpkg.json'
  if (-not (Test-Path $vcpkgJson)) { return $null }
  try {
    $json = Get-Content -Path $vcpkgJson -Raw | ConvertFrom-Json
    if ($json -and $json.'builtin-baseline') { return [string]$json.'builtin-baseline' }
  } catch {}
  return $null
}

function Ensure-VcpkgBaselineCommit([string]$VcpkgRoot, [string]$Baseline) {
  if (-not $Baseline -or $Baseline -eq '') { return }
  if (-not (Test-Path (Join-Path $VcpkgRoot '.git'))) { return }
  try {
    & git -C $VcpkgRoot cat-file -e "$Baseline^{commit}" 2>$null
    if ($LASTEXITCODE -eq 0) { return }
  } catch {}

  Info "vcpkg baseline commit fetch: $Baseline"
  try {
    & git -C $VcpkgRoot fetch --depth 1 origin $Baseline | Out-Null
  } catch {
    Warn "vcpkg baseline commit fetch failed: $Baseline"
  }
}

function Invoke-WithRetry(
  [scriptblock]$Action,
  [string]$Description,
  [int]$MaxAttempts = 3,
  [int]$InitialDelaySeconds = 3
) {
  for ($attempt = 1; $attempt -le $MaxAttempts; $attempt++) {
    try {
      & $Action
      if ($LASTEXITCODE -eq 0) { return }
      throw "$Description exited with code $LASTEXITCODE"
    } catch {
      if ($attempt -ge $MaxAttempts) { throw }
      $delay = [Math]::Min([int]($InitialDelaySeconds * [Math]::Pow(2, $attempt - 1)), 30)
      Warn ("{0} 실패 (시도 {1}/{2}): {3}" -f $Description, $attempt, $MaxAttempts, $_.Exception.Message)
      Info ("{0}초 후 재시도합니다." -f $delay)
      Start-Sleep -Seconds $delay
    }
  }
}

if (-not $Triplet -or $Triplet -eq '') {
  if ($IsWindows) { $Triplet = 'x64-windows' }
  else { $Triplet = 'x64-linux' }
}

if ($Features.Count -eq 0 -and $IsWindows) {
  # Default: install Windows dev dependencies declared in vcpkg.json.
  $Features = @('windows-dev')
}

if (-not (Test-Path $vcpkgRoot)) {
  Info "vcpkg 저장소가 없어 새로 클론합니다: $Repo -> $vcpkgRoot"
  # vcpkg manifest versioning(builtin-baseline)은 과거 커밋/트리를 필요로 하므로 shallow clone을 피한다.
  git clone $Repo $vcpkgRoot | Out-Null
} else {
  Info "기존 vcpkg 경로 사용: $vcpkgRoot"
}

# 기존 shallow clone이라면 전체 히스토리/오브젝트를 가져온다.
if (Test-Path (Join-Path $vcpkgRoot '.git/shallow')) {
  Info "vcpkg shallow clone 감지: 전체 히스토리 fetch 수행"
  try {
    & git -C $vcpkgRoot fetch --unshallow | Out-Null
  } catch {
    Warn "vcpkg unshallow fetch 실패. 네트워크/권한을 확인하세요."
  }
}

$baseline = Get-VcpkgBaseline $repoRoot
if ($baseline) {
  Ensure-VcpkgBaselineCommit $vcpkgRoot $baseline
}

$exeName = 'vcpkg'
if ($IsWindows) { $exeName = 'vcpkg.exe' }
$exePath = Join-Path $vcpkgRoot $exeName
if (-not (Test-Path $exePath)) {
  $bootstrap = 'bootstrap-vcpkg.sh'
  if ($IsWindows) { $bootstrap = 'bootstrap-vcpkg.bat' }
  $bootstrapPath = Join-Path $vcpkgRoot $bootstrap
  if (-not (Test-Path $bootstrapPath)) { Fail "bootstrap 스크립트를 찾지 못했습니다: $bootstrapPath" }
  Info "vcpkg bootstrap 실행: $bootstrapPath"
  Push-Location $vcpkgRoot
  try {
    Invoke-WithRetry -Description 'vcpkg bootstrap' -MaxAttempts 4 -InitialDelaySeconds 5 -Action {
      if ($IsWindows) {
        & $bootstrapPath
      } else {
        bash $bootstrapPath
      }
    }
  } finally {
    Pop-Location
  }
  if (-not (Test-Path $exePath)) { Fail "vcpkg 실행파일 생성 실패: $exePath" }
}

if (-not $SkipInstall) {
  $args = @('install', '--triplet', $Triplet)
  foreach ($f in $Features) {
    if ($f -and $f.Trim() -ne '') {
      $args += @('--x-feature', $f)
    }
  }
  Info ("manifest 의존성 설치(vcpkg {0})" -f ($args -join ' '))
  Push-Location $repoRoot
  try {
    & $exePath @args | Out-Host
  } finally {
    Pop-Location
  }
  if ($LASTEXITCODE -ne 0) { Fail 'vcpkg install 실패' }
}

Write-Output (Resolve-Path $vcpkgRoot).Path
