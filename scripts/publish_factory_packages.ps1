<#
  Factory package publish helper
  - Builds the packaged factory targets in Release mode.
  - Installs the package surface into a staged prefix bundle.
  - Emits a small manifest for downstream artifact consumers.
#>
[CmdletBinding()]
param(
  [string]$BuildDir = "",
  [string]$OutputRoot = "artifacts/factory-packages",
  [switch]$Zip,
  [switch]$CleanOutput
)

$ErrorActionPreference = 'Stop'

function Info($msg){ Write-Host "[info] $msg" -ForegroundColor Cyan }
function Warn($msg){ Write-Host "[warn] $msg" -ForegroundColor Yellow }
function Fail($msg){ Write-Host "[fail] $msg" -ForegroundColor Red; exit 1 }

function Ensure-Directory([string]$Path) {
  if (-not (Test-Path $Path)) {
    New-Item -ItemType Directory -Path $Path -Force | Out-Null
  }
  return (Resolve-Path $Path).Path
}

function Get-VersionString([string]$VersionHeaderPath) {
  if (-not (Test-Path $VersionHeaderPath)) {
    Fail "version header를 찾을 수 없습니다: $VersionHeaderPath"
  }
  $content = Get-Content -Path $VersionHeaderPath -Raw
  $match = [regex]::Match($content, 'version_string\(\) noexcept \{ return "([0-9]+\.[0-9]+\.[0-9]+)"')
  if (-not $match.Success) {
    Fail "version_string() 값을 파싱하지 못했습니다: $VersionHeaderPath"
  }
  return $match.Groups[1].Value
}

function Get-GitValue([string[]]$Args, [string]$Fallback) {
  $git = Get-Command git -ErrorAction SilentlyContinue
  if (-not $git) {
    $git = Get-Command git.exe -ErrorAction SilentlyContinue
  }
  if (-not $git) { return $Fallback }
  try {
    $value = & $git.Source @Args 2>$null
    if ($LASTEXITCODE -ne 0) { return $Fallback }
    if (-not $value) { return $Fallback }
    return ($value | Select-Object -First 1).ToString().Trim()
  } catch {
    return $Fallback
  }
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Push-Location $repoRoot
try {
  $isWindowsHost = $false
  try { if ($IsWindows) { $isWindowsHost = $true } } catch { }
  if (-not $isWindowsHost) { $isWindowsHost = ($env:OS -like '*Windows*') }

  if (-not $BuildDir -or $BuildDir -eq '') {
    if ($isWindowsHost) { $BuildDir = 'build-windows' }
    else { $BuildDir = 'build-linux-conan' }
  }

  $config = 'Release'
  $arch = if ([Environment]::Is64BitOperatingSystem) { 'x64' } else { 'x86' }
  $platform = if ($isWindowsHost) { 'windows' } else { 'linux' }
  $version = Get-VersionString (Join-Path $repoRoot 'core/include/server/core/api/version.hpp')
  $gitHash = Get-GitValue @('rev-parse', 'HEAD') 'unknown'
  $gitDescribe = Get-GitValue @('describe', '--always', '--dirty') 'unknown'

  $bundleName = "dynaxis-factory-packages-$platform-$arch-$($config.ToLowerInvariant())-$version"
  $outputRootPath = Ensure-Directory (Join-Path $repoRoot $OutputRoot)
  $bundleDir = Join-Path $outputRootPath $bundleName
  $prefixDir = Join-Path $bundleDir 'prefix'
  $manifestPath = Join-Path $bundleDir 'manifest.json'
  $zipPath = "$bundleDir.zip"

  if ($CleanOutput) {
    if (Test-Path $bundleDir) { Remove-Item -Recurse -Force $bundleDir }
    if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
  }

  if (Test-Path $bundleDir) { Remove-Item -Recurse -Force $bundleDir }
  New-Item -ItemType Directory -Path $bundleDir -Force | Out-Null

  $buildScript = Join-Path $repoRoot 'scripts/build.ps1'
  if (-not (Test-Path $buildScript)) {
    Fail "build script를 찾을 수 없습니다: $buildScript"
  }

  $bootstrapTarget = 'server_storage_pg_factory'
  Info "빌드/구성 bootstrap: $bootstrapTarget ($config)"
  & $buildScript -Config $config -BuildDir $BuildDir -Target $bootstrapTarget
  if ($LASTEXITCODE -ne 0) {
    Fail "factory target 빌드 실패: $bootstrapTarget"
  }

  $followupTargets = @('server_storage_redis_factory')
  foreach ($target in $followupTargets) {
    Info "추가 빌드: $target ($config)"
    & cmake --build $BuildDir --config $config --target $target --parallel
    if ($LASTEXITCODE -ne 0) {
      Fail "factory target 추가 빌드 실패: $target"
    }
  }

  Info "설치 prefix 생성: $prefixDir"
  & cmake --install $BuildDir --config $config --prefix $prefixDir
  if ($LASTEXITCODE -ne 0) {
    Fail "factory package install 실패"
  }

  $manifest = [ordered]@{
    bundle_name = $bundleName
    version = $version
    config = $config
    platform = $platform
    arch = $arch
    git_hash = $gitHash
    git_describe = $gitDescribe
    created_utc = (Get-Date).ToUniversalTime().ToString('o')
    build_dir = $BuildDir
    install_prefix = 'prefix'
    packages = @(
      'server_core',
      'server_storage_pg_factory',
      'server_storage_redis_factory'
    )
    exclusions = @(
      'server_storage_pg compatibility umbrella',
      'server_storage_redis compatibility umbrella',
      'server_state_redis_factory',
      'app-local helper targets'
    )
  }
  $manifest | ConvertTo-Json -Depth 5 | Set-Content -Path $manifestPath -Encoding utf8

  if ($Zip) {
    if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
    Compress-Archive -Path (Join-Path $bundleDir '*') -DestinationPath $zipPath
    Info "zip 생성: $zipPath"
  }

  Info "bundle 생성: $bundleDir"
  Write-Output "BUNDLE_DIR=$bundleDir"
  if ($Zip) {
    Write-Output "BUNDLE_ZIP=$zipPath"
  }
}
finally {
  Pop-Location
}
