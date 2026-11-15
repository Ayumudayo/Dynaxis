<#
  빌드/구성 스크립트 (PowerShell)
  - Conan2 is the default and only supported dependency provider.
  - Windows/Linux configure+build both go through Conan-generated toolchains.
  - Linux runtime remains Docker-first for stack execution.
#>
[CmdletBinding()]
param(
  [string]$Generator = "",
  [ValidateSet('Debug','Release','RelWithDebInfo','MinSizeRel')]
  [string]$Config = "RelWithDebInfo",
  [string]$BuildDir = "",
  [string]$Target = "",
  [switch]$ClientOnly,
  [switch]$Clean,
  [string]$InstallPrefix = "",
  [ValidateSet('none','server','client','both')]
  [string]$Run = 'none',
  [int]$Port = 5000,
  [switch]$UseConan,
  [switch]$ReleasePackage,
  [string]$ReleaseOutput = "artifacts",
  [string[]]$ReleaseTargets = @('server_app','gateway_app','wb_worker'),
  [switch]$ReleaseZip,
  [int]$MaxJobs = 0
)

$ErrorActionPreference = 'Stop'

# UTF-8 콘솔 강제(한글 출력/입력)
try { chcp 65001 | Out-Null } catch {}
try {
  $enc = New-Object System.Text.UTF8Encoding $false
  [Console]::OutputEncoding = $enc
  [Console]::InputEncoding  = $enc
  $Global:OutputEncoding = $enc
} catch {}

function Info($msg){ Write-Host "[info] $msg" -ForegroundColor Cyan }
function Warn($msg){ Write-Host "[warn] $msg" -ForegroundColor Yellow }
function Fail($msg){ Write-Host "[fail] $msg" -ForegroundColor Red; exit 1 }

function Ensure-Directory([string]$Path) {
  if (-not $Path -or $Path -eq '') { return (Resolve-Path '.').Path }
  if (-not (Test-Path $Path)) { New-Item -ItemType Directory -Path $Path -Force | Out-Null }
  return (Resolve-Path $Path).Path
}

function Resolve-BinaryPath([string]$Name, [bool]$IsWindows) {
  if (-not $Name -or $Name -eq '') { return $null }
  $ext = ''
  if ($IsWindows) { $ext = '.exe' }
  $candidates = @(
    (Join-Path $BuildDir "$Name$ext"),
    (Join-Path $BuildDir (Join-Path $Config "$Name$ext"))
  )
  foreach ($sub in @('server','gateway','load_balancer','client_gui','tools','wb')) {
    $candidates += (Join-Path $BuildDir (Join-Path $sub "$Name$ext"))
    $candidates += (Join-Path $BuildDir (Join-Path $sub (Join-Path $Config "$Name$ext")))
  }
  foreach ($candidate in $candidates) {
    if ($candidate -and (Test-Path $candidate)) { return (Resolve-Path $candidate).Path }
  }
  try {
    $filter = "*$Name$ext"
    $found = Get-ChildItem -Path $BuildDir -Recurse -File -Filter $filter -ErrorAction SilentlyContinue |
             Sort-Object LastWriteTime -Descending |
             Select-Object -First 1
    if ($found) { return $found.FullName }
  } catch {}
  return $null
}

$onWindows = $false
try { if ($IsWindows) { $onWindows = $true } } catch { }
if (-not $onWindows) { $onWindows = ($PSVersionTable.PSEdition -eq 'Desktop' -or $env:OS -like '*Windows*') }

if ($Clean) { Fail "Use cmake --build <directory> --target clean; dependency metadata is retained." }
if ($ClientOnly) { $Config = 'Release'; $profileName = 'client' } else { $profileName = 'full' }
if (-not $BuildDir) {
  $platformName = if ($onWindows) { 'windows' } elseif ($IsMacOS) { 'macos' } else { 'linux' }
  $BuildDir = "build/$platformName/$profileName/$($Config.ToLowerInvariant())"
}
$commonArgs = @((Join-Path $PSScriptRoot 'build.py'), '--profile', $profileName,
  '--config', $Config, '--build-dir', $BuildDir, '--step', 'all')
if ($Target) { $commonArgs += @('--target', $Target) }
if ($MaxJobs -gt 0) { $commonArgs += @('--jobs', $MaxJobs) }
if ($Generator) { $commonArgs += @('--generator', $Generator) }
& python @commonArgs
if ($LASTEXITCODE -ne 0) { Fail 'Conan/CMake build failed' }

if ($InstallPrefix -and $InstallPrefix -ne '') {
  Info "설치 중... ($InstallPrefix)"
  & cmake --install $BuildDir --config $Config --prefix $InstallPrefix
  if ($LASTEXITCODE -ne 0) { Fail "설치 실패" }
}

if ($ReleasePackage) {
  $releaseRoot = Ensure-Directory $ReleaseOutput
  $releaseDirName = "release-$Config"
  $releaseDir = Join-Path $releaseRoot $releaseDirName
  if (Test-Path $releaseDir) { Remove-Item -Recurse -Force $releaseDir }
  New-Item -ItemType Directory -Path $releaseDir | Out-Null
  $uniqueTargets = $ReleaseTargets | Where-Object { $_ -and $_.Trim() -ne '' } | Select-Object -Unique
  foreach ($name in $uniqueTargets) {
    $binary = Resolve-BinaryPath $name $onWindows
    if (-not $binary) {
      Warn "release bundle 대상 바이너리를 찾을 수 없습니다: $name"
      continue
    }
    $dest = Join-Path $releaseDir (Split-Path $binary -Leaf)
    Copy-Item -Path $binary -Destination $dest -Force
  }
  if (Test-Path 'README.md') {
    Copy-Item -Path 'README.md' -Destination (Join-Path $releaseDir 'README.md') -Force
  }
  if ($ReleaseZip) {
    $zipPath = "$releaseDir.zip"
    if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
    Compress-Archive -Path (Join-Path $releaseDir '*') -DestinationPath $zipPath
    Info "release archive 생성: $zipPath"
  } else {
    Info "release bundle 생성: $releaseDir"
  }
}

if ($Run -ne 'none') {
  $Generator = if ($onWindows) { 'Visual Studio 17 2022' } else { 'Ninja' }
  $exe = ''
  if ($Run -eq 'server') {
    if ($onWindows -and $Generator -like 'Visual Studio*') { $exe = Join-Path $BuildDir (Join-Path 'server' (Join-Path $Config 'server_app.exe')) }
    else { $exe = Join-Path $BuildDir 'server/server_app' }
    if (-not (Test-Path $exe)) { $exe = Join-Path $BuildDir 'server_app' }
    if (-not (Test-Path $exe)) { Fail "server_app 실행 파일을 찾을 수 없습니다." }
    Info "서버 실행: $exe $Port"
    & $exe $Port
  }
  elseif ($Run -eq 'client') {

    $candidates = @(
      (Join-Path $BuildDir (Join-Path 'client_gui' (Join-Path $Config 'client_gui.exe'))),
      (Join-Path $BuildDir (Join-Path $Config 'client_gui.exe')),
      (Join-Path $BuildDir 'client_gui.exe'),
      (Join-Path $BuildDir 'client_gui/client_gui')
    )
    foreach ($candidate in $candidates) {
      if (Test-Path $candidate) {
        $exe = $candidate
        break
      }
    }
    if (-not $exe -or -not (Test-Path $exe)) { Fail "client_gui 실행 파일을 찾을 수 없습니다." }
    Info "클라이언트 실행: $exe 127.0.0.1 $Port"
    & $exe '127.0.0.1' $Port
  }
  elseif ($Run -eq 'both') {
    # 서버 실행(백그라운드), 잠시 대기 후 클라이언트 실행(localhost:5000 기본값)
    $serverExe = ''
    if ($onWindows -and $Generator -like 'Visual Studio*') { $serverExe = Join-Path $BuildDir (Join-Path 'server' (Join-Path $Config 'server_app.exe')) }
    else { $serverExe = Join-Path $BuildDir 'server/server_app' }
    if (-not (Test-Path $serverExe)) { $serverExe = Join-Path $BuildDir 'server_app' }
    if (-not (Test-Path $serverExe)) { Fail "server_app 실행 파일을 찾을 수 없습니다." }

    if (-not $onWindows) {
      Fail "both 모드는 Windows에서만 지원됩니다. client_gui를 사용하세요."
    }
    $clientExe = ''
    $clientCandidates = @(
      (Join-Path $BuildDir (Join-Path 'client_gui' (Join-Path $Config 'client_gui.exe'))),
      (Join-Path $BuildDir (Join-Path $Config 'client_gui.exe')),
      (Join-Path $BuildDir 'client_gui.exe'),
      (Join-Path $BuildDir 'client_gui/client_gui')
    )
    foreach ($candidate in $clientCandidates) {
      if (Test-Path $candidate) {
        $clientExe = $candidate
        break
      }
    }
    if (-not $clientExe -or -not (Test-Path $clientExe)) { Fail "client_gui 실행 파일을 찾을 수 없습니다." }

    Info "서버 시작: $serverExe 5000 (백그라운드)"
    if ($onWindows) {
      Start-Process -FilePath $serverExe -ArgumentList '5000' -WindowStyle Hidden | Out-Null
    } else {
      Start-Process -FilePath $serverExe -ArgumentList '5000' | Out-Null
    }
    Start-Sleep -Seconds 1
    Info "클라이언트 시작: $clientExe (localhost:5000)"
    & $clientExe
  }
}

Info "완료"
