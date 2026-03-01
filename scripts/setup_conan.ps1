<#
  Conan2 bootstrap + install helper for Knights
  - Resolves host/build profiles from repository defaults.
  - Runs `conan profile detect --force`.
  - Runs `conan install` into <buildDir>/conan unless -SkipInstall is set.
#>
[CmdletBinding()]
param(
  [ValidateSet('Debug','Release','RelWithDebInfo','MinSizeRel')]
  [string]$Config = 'RelWithDebInfo',
  [ValidateSet('windows-dev','windows-client')]
  [string]$Feature = 'windows-dev',
  [string]$BuildDir = '',
  [string]$HostProfile = '',
  [string]$BuildProfile = '',
  [string]$LockfilePath = 'conan.lock',
  [switch]$NoLockfile,
  [switch]$SkipInstall
)

$ErrorActionPreference = 'Stop'

function Info($msg){ Write-Host "[info] $msg" -ForegroundColor Cyan }
function Warn($msg){ Write-Host "[warn] $msg" -ForegroundColor Yellow }
function Fail($msg){ Write-Host "[fail] $msg" -ForegroundColor Red; exit 1 }

function Resolve-PathOrFail([string]$Path) {
  if (-not (Test-Path $Path)) { Fail "경로를 찾을 수 없습니다: $Path" }
  return (Resolve-Path $Path).Path
}

function Resolve-AbsolutePath([string]$RepoRoot, [string]$Path) {
  if (-not $Path -or $Path -eq '') { return '' }
  if ([System.IO.Path]::IsPathRooted($Path)) {
    return $Path
  }
  return (Join-Path $RepoRoot $Path)
}

function Resolve-ConfigFlavor([string]$BuildConfig) {
  if ($BuildConfig -eq 'Debug') { return 'debug' }
  if ($BuildConfig -eq 'Release') { return 'release' }
  if ($BuildConfig -eq 'RelWithDebInfo' -or $BuildConfig -eq 'MinSizeRel') {
    Warn "Conan dependency profile은 '$BuildConfig'를 Release 계열로 매핑합니다."
    return 'release'
  }
  return 'release'
}

if (-not (Get-Variable -Name IsWindows -ErrorAction SilentlyContinue)) {
  $IsWindows = $env:OS -like '*Windows*'
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$flavor = Resolve-ConfigFlavor $Config

if (-not $BuildDir -or $BuildDir -eq '') {
  if ($IsWindows) {
    if ($Feature -eq 'windows-client') { $BuildDir = 'build-windows-client-conan' }
    else { $BuildDir = 'build-windows-conan' }
  } else {
    $BuildDir = 'build-linux-conan'
  }
}

$resolvedBuildDir = Resolve-AbsolutePath $repoRoot $BuildDir
if (-not (Test-Path $resolvedBuildDir)) {
  New-Item -ItemType Directory -Path $resolvedBuildDir -Force | Out-Null
}
$resolvedBuildDir = (Resolve-Path $resolvedBuildDir).Path

if (-not $HostProfile -or $HostProfile -eq '') {
  if ($IsWindows) {
    if ($flavor -eq 'debug') { $HostProfile = 'conan/profiles/host/windows-msvc-debug' }
    else { $HostProfile = 'conan/profiles/host/windows-msvc-release' }
  } else {
    if ($flavor -eq 'debug') { $HostProfile = 'conan/profiles/host/linux-gcc-debug' }
    else { $HostProfile = 'conan/profiles/host/linux-gcc-release' }
  }
}
if (-not $BuildProfile -or $BuildProfile -eq '') {
  $BuildProfile = 'conan/profiles/build/default'
}

$hostProfilePath = Resolve-AbsolutePath $repoRoot $HostProfile
$buildProfilePath = Resolve-AbsolutePath $repoRoot $BuildProfile
$hostProfilePath = Resolve-PathOrFail $hostProfilePath
$buildProfilePath = Resolve-PathOrFail $buildProfilePath

$resolvedLockfilePath = ''
if (-not $NoLockfile -and $LockfilePath -and $LockfilePath -ne '') {
  $resolvedLockfilePath = Resolve-AbsolutePath $repoRoot $LockfilePath
}

$conanOutputDir = Join-Path $resolvedBuildDir 'conan'
if (-not (Test-Path $conanOutputDir)) {
  New-Item -ItemType Directory -Path $conanOutputDir -Force | Out-Null
}
$conanOutputDir = (Resolve-Path $conanOutputDir).Path

$conanCmd = Get-Command conan -ErrorAction SilentlyContinue
if (-not $conanCmd) {
  Fail "conan 실행 파일을 찾지 못했습니다. Conan2를 설치한 뒤 다시 시도하세요."
}

$conanHome = Join-Path $env:USERPROFILE '.conan2'
$defaultProfile = Join-Path $conanHome 'profiles/default'
if (-not (Test-Path $defaultProfile)) {
  Info "conan profile detect --force"
  & $conanCmd.Source profile detect --force | Out-Host
  if ($LASTEXITCODE -ne 0) {
    Fail "conan profile detect 실패"
  }
} else {
  Info "기존 Conan default profile 재사용: $defaultProfile"
}

if (-not $SkipInstall) {
  $installArgs = @(
    'install', $repoRoot,
    '--output-folder', $conanOutputDir,
    '--build=missing',
    '--profile:host', $hostProfilePath,
    '--profile:build', $buildProfilePath,
    '--options:host', "&:knights_feature=$Feature"
  )

  if ($resolvedLockfilePath -and $resolvedLockfilePath -ne '') {
    if (Test-Path $resolvedLockfilePath) {
      Info "lockfile 사용: $resolvedLockfilePath"
      $installArgs += @('--lockfile', $resolvedLockfilePath)
    } else {
      Warn "lockfile이 없어 새로 생성합니다: $resolvedLockfilePath"
      $installArgs += @('--lockfile-out', $resolvedLockfilePath)
    }
  }

  Info "conan install (feature=$Feature, config=$Config)"
  & $conanCmd.Source @installArgs | Out-Host
  if ($LASTEXITCODE -ne 0) {
    Fail "conan install 실패"
  }

  $userPresetsPath = Join-Path $repoRoot 'CMakeUserPresets.json'
  if (Test-Path $userPresetsPath) {
    try {
      $userPresets = Get-Content -Raw -Path $userPresetsPath | ConvertFrom-Json
      if ($userPresets.vendor -and $null -ne $userPresets.vendor.conan) {
        Remove-Item -Path $userPresetsPath -Force
        Info "conan-generated CMakeUserPresets.json 제거: $userPresetsPath"
      }
    }
    catch {
      Warn "CMakeUserPresets.json 확인 중 경고: $($_.Exception.Message)"
    }
  }
}

Write-Output $conanOutputDir
