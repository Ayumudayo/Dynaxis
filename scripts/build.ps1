<#
  빌드/구성 스크립트 (PowerShell)
  - Windows/MSVC default. Dependencies come from vcpkg manifest features (see vcpkg.json).
  - Linux runtime is Docker (system packages + source builds in Dockerfile.base). This script does not auto-run vcpkg on Linux.
#>
[CmdletBinding()]
param(
  [string]$Generator = "",
  [ValidateSet('Debug','Release','RelWithDebInfo','MinSizeRel')]
  [string]$Config = "RelWithDebInfo",
  [string]$BuildDir = "",
  [string]$Target = "",
  [switch]$Clean,
  [string]$InstallPrefix = "",
  [ValidateSet('none','server','client','both')]
  [string]$Run = 'none',
  [int]$Port = 5000,
  [switch]$UseVcpkg,
  [string]$VcpkgTriplet = "",
  [switch]$ReleasePackage,
  [string]$ReleaseOutput = "artifacts",
  [string[]]$ReleaseTargets = @('server_app','gateway_app','dev_chat_cli','wb_worker'),
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
  foreach ($sub in @('server','gateway','load_balancer','devclient','tools','wb')) {
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

if (-not $BuildDir -or $BuildDir -eq '') {
  # CMakePresets.json의 기본 binaryDir과 일치시키는 것을 전제로 한다.
  if ($onWindows) { $BuildDir = 'build-windows' } else { $BuildDir = 'build-linux' }
}
Info "BuildDir=$BuildDir"

if ($Clean) {
  if (Test-Path $BuildDir) { Info "빌드 폴더 정리: $BuildDir"; Remove-Item -Recurse -Force $BuildDir }
}

# 제너레이터 설정 및 프리셋 선택
if (-not $Generator -or $Generator -eq '') {
  if ($onWindows) { 
      $Preset = "windows"
      $BuildPreset = "windows-debug"
      if ($Config -eq "RelWithDebInfo") { $BuildPreset = "windows-release" }
  } else { 
      $Preset = "linux"
      $BuildPreset = "linux-debug"
      if ($Config -eq "RelWithDebInfo") { $BuildPreset = "linux-release" }
  }
}

$cmakeArgs = @('--preset', $Preset)

# vcpkg toolchain (local checkout)
$vcpkgJsonExists = Test-Path 'vcpkg.json'
if (-not $VcpkgTriplet -or $VcpkgTriplet -eq '') {
  if ($onWindows) { $VcpkgTriplet = 'x64-windows' } else { $VcpkgTriplet = 'x64-linux' }
}
$shouldUseVcpkg = $UseVcpkg -or ($onWindows -and $vcpkgJsonExists)
if ($UseVcpkg -and -not $onWindows) {
  Warn "UseVcpkg requested on non-Windows; Linux presets are configured for system dependencies (no vcpkg toolchain)."
}

if ($shouldUseVcpkg) {
  $setupScript = Join-Path $PSScriptRoot 'setup_vcpkg.ps1'
  if (-not (Test-Path $setupScript)) { Fail "setup_vcpkg.ps1 스크립트를 찾을 수 없습니다: $setupScript" }
  # CMake preset(toolchain) will run manifest install; here we only ensure vcpkg is cloned/bootstrapped.
  $vcpkgRoot = & $setupScript -Triplet $VcpkgTriplet -SkipInstall
  if (-not $vcpkgRoot) { Fail "vcpkg root 경로를 확인할 수 없습니다." }
  Info "vcpkg root: $vcpkgRoot"
  # Presets handle toolchain file, but we might need to ensure vcpkg is bootstrapped
}

Info "CMake 구성 중... (Preset: $Preset)"
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { Fail "CMake 구성 실패" }

Info "빌드 중... (Preset: $BuildPreset)"

$buildArgs = @()
if ($Target -and $Target -ne '') {
  # Visual Studio generator는 타깃별 .vcxproj가 서브 디렉터리에 생성될 수 있어
  # `cmake --build --preset ... --target <name>`가 실패할 수 있다.
  # (예: build-windows/gateway/gateway_app.vcxproj)
  # 이 경우 해당 .vcxproj가 있는 디렉터리를 빌드 디렉터리로 사용한다.
  if ($onWindows) {
    $resolvedBuildDir = $BuildDir
    try { $resolvedBuildDir = (Resolve-Path $BuildDir).Path } catch {}
    $proj = $null
    try {
      $proj = Get-ChildItem -Path $resolvedBuildDir -Recurse -File -Filter "$Target.vcxproj" -ErrorAction SilentlyContinue |
              Sort-Object FullName |
              Select-Object -First 1
    } catch {}

    if ($proj) {
      $buildArgs = @('--build', $proj.DirectoryName, '--config', $Config, '--target', $Target)
    } else {
      $buildArgs = @('--build', '--preset', $BuildPreset, '--target', $Target)
    }
  } else {
    $buildArgs = @('--build', '--preset', $BuildPreset, '--target', $Target)
  }
} else {
  $buildArgs = @('--build', '--preset', $BuildPreset)
}

if ($MaxJobs -gt 0) {
  $buildArgs += @('--parallel', $MaxJobs)
} else {
  $buildArgs += @('--parallel')
}

& cmake @buildArgs
if ($LASTEXITCODE -ne 0) { Fail "빌드 실패" }

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
  $exe = ''
  if ($Run -eq 'server') {
    if ($onWindows -and $Generator -like 'Visual Studio*') { $exe = Join-Path $BuildDir (Join-Path $Config 'server_app.exe') }
    else { $exe = Join-Path $BuildDir 'server/server_app' }
    if (-not (Test-Path $exe)) { $exe = Join-Path $BuildDir 'server_app' }
    if (-not (Test-Path $exe)) { Fail "server_app 실행 파일을 찾을 수 없습니다." }
    Info "서버 실행: $exe $Port"
    & $exe $Port
  }
  elseif ($Run -eq 'client') {
    if ($onWindows -and $Generator -like 'Visual Studio*') { $exe = Join-Path $BuildDir (Join-Path $Config 'dev_chat_cli.exe') }
    else { $exe = Join-Path $BuildDir 'devclient/dev_chat_cli' }
    if (-not (Test-Path $exe)) { $exe = Join-Path $BuildDir 'dev_chat_cli' }
    if (-not (Test-Path $exe)) { Fail "dev_chat_cli 실행 파일을 찾을 수 없습니다." }
    Info "클라이언트 실행: $exe 127.0.0.1 $Port"
    & $exe '127.0.0.1' $Port
  }
  elseif ($Run -eq 'both') {
    # 서버 실행(백그라운드), 잠시 대기 후 클라이언트 실행(localhost:5000 기본값)
    $serverExe = ''
    if ($onWindows -and $Generator -like 'Visual Studio*') { $serverExe = Join-Path $BuildDir (Join-Path $Config 'server_app.exe') }
    else { $serverExe = Join-Path $BuildDir 'server/server_app' }
    if (-not (Test-Path $serverExe)) { $serverExe = Join-Path $BuildDir 'server_app' }
    if (-not (Test-Path $serverExe)) { Fail "server_app 실행 파일을 찾을 수 없습니다." }

    $clientExe = ''
    if ($onWindows -and $Generator -like 'Visual Studio*') { $clientExe = Join-Path $BuildDir (Join-Path $Config 'dev_chat_cli.exe') }
    else { $clientExe = Join-Path $BuildDir 'devclient/dev_chat_cli' }
    if (-not (Test-Path $clientExe)) { $clientExe = Join-Path $BuildDir 'dev_chat_cli' }
    if (-not (Test-Path $clientExe)) { Fail "dev_chat_cli 실행 파일을 찾을 수 없습니다." }

    Info "서버 시작: $serverExe 5000 (백그라운드)"
    if ($onWindows) {
      Start-Process -FilePath $serverExe -ArgumentList '5000' -WindowStyle Minimized | Out-Null
    } else {
      Start-Process -FilePath $serverExe -ArgumentList '5000' | Out-Null
    }
    Start-Sleep -Seconds 1
    Info "클라이언트 시작: $clientExe (localhost:5000)"
    & $clientExe
  }
}

Info "완료"

