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

if ($ClientOnly -and -not $onWindows) {
  Warn "ClientOnly mode is Windows-only. Falling back to the default preset selection."
  $ClientOnly = $false
}

if ($ClientOnly) {
  if ($Config -ne 'Release') {
    Warn "ClientOnly mode enforces Release build to reduce artifact size. Config '$Config' -> 'Release'"
    $Config = 'Release'
  }
  if (-not $Target -or $Target -eq '') {
    $Target = 'client_gui'
    Info "ClientOnly mode: default target set to client_gui"
  }
}

if (-not $PSBoundParameters.ContainsKey('UseConan')) {
  $UseConan = $true
}
if (-not $UseConan) {
  Warn "비-Conan 빌드 경로는 제거되었습니다. Conan 모드로 강제 전환합니다."
  $UseConan = $true
}

if (-not $BuildDir -or $BuildDir -eq '') {
  # CMakePresets.json의 기본 binaryDir과 일치시키는 것을 전제로 한다.
  if ($onWindows) {
    if ($ClientOnly) {
      $BuildDir = 'build-windows-client'
    }
    else {
      $BuildDir = 'build-windows'
    }
  } else {
    $BuildDir = 'build-linux-conan'
  }
}
Info "BuildDir=$BuildDir"

if ($Clean) {
  if (Test-Path $BuildDir) { Info "빌드 폴더 정리: $BuildDir"; Remove-Item -Recurse -Force $BuildDir }
}

# 제너레이터 설정 및 프리셋 선택
if (-not $Generator -or $Generator -eq '') {
  if ($onWindows) {
    if ($ClientOnly) { $Preset = 'windows-client' }
    else { $Preset = 'windows' }
  } else {
    if ($Config -eq 'Debug') { $Preset = 'linux-conan' }
    else { $Preset = 'linux-conan-release' }
  }
}

$cmakeArgs = @()
$useVsBuildLayout = $false

$setupConanScript = Join-Path $PSScriptRoot 'setup_conan.ps1'
if (-not (Test-Path $setupConanScript)) { Fail "setup_conan.ps1 스크립트를 찾을 수 없습니다: $setupConanScript" }

$conanFeature = 'windows-dev'
if ($ClientOnly) { $conanFeature = 'windows-client' }
$conanLockfile = 'conan.lock'
if ($conanFeature -eq 'windows-client') { $conanLockfile = 'conan-client.lock' }

$conanOutputDir = & $setupConanScript -Config $Config -Feature $conanFeature -BuildDir $BuildDir -LockfilePath $conanLockfile
if (-not $conanOutputDir) { Fail "Conan output 경로를 확인할 수 없습니다." }

$toolchainCandidates = @(
  (Join-Path $conanOutputDir 'conan_toolchain.cmake'),
  (Join-Path $conanOutputDir 'build\generators\conan_toolchain.cmake'),
  (Join-Path $conanOutputDir 'build\Debug\generators\conan_toolchain.cmake'),
  (Join-Path $conanOutputDir 'build\Release\generators\conan_toolchain.cmake')
)
$toolchainFile = $null
foreach ($candidate in $toolchainCandidates) {
  if (Test-Path $candidate) {
    $toolchainFile = $candidate
    break
  }
}
if (-not $toolchainFile) {
  Fail "Conan toolchain 파일을 찾지 못했습니다. 확인한 경로: $($toolchainCandidates -join ', ')"
}

$useConanPreset = ($Preset -and $Preset -ne '')
if ($useConanPreset) {
  $useVsBuildLayout = $onWindows
  $cmakeArgs = @('--preset', $Preset)
  if ($onWindows) {
    # Conan install은 단일 build_type 바이너리 그래프를 생성하므로,
    # Multi-config(VS)에서도 해당 구성만 생성 대상으로 제한한다.
    $cmakeArgs += "-DCMAKE_CONFIGURATION_TYPES=$Config"
  }
  Info "CMake 구성 중... (Conan preset: $Preset)"
} else {
  if ($onWindows) { $Generator = 'Visual Studio 17 2022' }
  else { $Generator = 'Ninja' }
  $useVsBuildLayout = ($onWindows -and $Generator -like 'Visual Studio*')

  $cmakeArgs = @('-S', '.', '-B', $BuildDir, '-G', $Generator, "-DCMAKE_TOOLCHAIN_FILE=$toolchainFile")
  if ($onWindows) {
    if ($Generator -like 'Visual Studio*') {
      $cmakeArgs += @('-A', 'x64')
    } else {
      $cmakeArgs += "-DCMAKE_BUILD_TYPE=$Config"
    }
  } else {
    $cmakeArgs += "-DCMAKE_BUILD_TYPE=$Config"
  }

  $cmakeArgs += @(
    '-DCMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO=Release',
    '-DCMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL=Release'
  )

  if ($ClientOnly) {
    $cmakeArgs += @(
      '-DBUILD_SERVER_STACK=OFF',
      '-DBUILD_GATEWAY_APP=OFF',
      '-DBUILD_SERVER_TESTS=OFF',
      '-DBUILD_WRITE_BEHIND_TOOLS=OFF',
      '-DBUILD_ADMIN_APP=OFF',
      '-DBUILD_MIGRATIONS_RUNNER=OFF'
    )
  }

  Info "CMake 구성 중... (Conan + Generator: $Generator)"
}

& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { Fail "CMake 구성 실패" }

Info "빌드 중... (Directory: $BuildDir)"

$buildArgs = @()
if ($Target -and $Target -ne '') {
  if ($useVsBuildLayout) {
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
      $buildArgs = @('--build', $BuildDir, '--config', $Config, '--target', $Target)
    }
  } else {
    $buildArgs = @('--build', $BuildDir, '--target', $Target)
  }
} else {
  $buildArgs = @('--build', $BuildDir)
  if ($useVsBuildLayout) {
    $buildArgs += @('--config', $Config)
  }
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
    if (-not $onWindows) {
      Fail "클라이언트 실행은 Windows에서만 지원됩니다. client_gui를 사용하세요."
    }
    $candidates = @(
      (Join-Path $BuildDir (Join-Path 'client_gui' (Join-Path $Config 'client_gui.exe'))),
      (Join-Path $BuildDir (Join-Path $Config 'client_gui.exe')),
      (Join-Path $BuildDir 'client_gui.exe')
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
    if ($onWindows -and $Generator -like 'Visual Studio*') { $serverExe = Join-Path $BuildDir (Join-Path $Config 'server_app.exe') }
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
      (Join-Path $BuildDir 'client_gui.exe')
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

