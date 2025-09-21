<#
  빌드/구성 스크립트 (PowerShell)
  - Windows/MSVC 기본. 환경변수 또는 인자로 BOOST 경로 지정 가능.
  - WSL/Linux에서도 사용 가능하지만, 리눅스용 Boost가 필요합니다.
#>
[CmdletBinding()]
param(
  [string]$Generator = "",
  [ValidateSet('Debug','Release','RelWithDebInfo','MinSizeRel')]
  [string]$Config = "RelWithDebInfo",
  [string]$BuildDir = "",
  [string]$BoostRoot = "",
  [string]$Target = "",
  [switch]$Clean,
  [string]$InstallPrefix = "",
[ValidateSet('none','server','client','both')]
[string]$Run = 'none',
  [int]$Port = 5000,
  [switch]$UseVcpkg,
  [string]$VcpkgTriplet = "",
  [switch]$NoVcpkgAutoInstall
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

$onWindows = $false
try { if ($IsWindows) { $onWindows = $true } } catch { }
if (-not $onWindows) { $onWindows = ($PSVersionTable.PSEdition -eq 'Desktop' -or $env:OS -like '*Windows*') }

if (-not $BuildDir -or $BuildDir -eq '') {
  if ($onWindows) { $BuildDir = 'build-msvc' } else { $BuildDir = 'build-linux' }
}
Info "BuildDir=$BuildDir"

if ($Clean) {
  if (Test-Path $BuildDir) { Info "빌드 폴더 정리: $BuildDir"; Remove-Item -Recurse -Force $BuildDir }
}

# Boost 경로 설정 (선택)
if (-not $BoostRoot -or $BoostRoot -eq '') {
  if ($env:BOOST_ROOT) { $BoostRoot = $env:BOOST_ROOT }
  elseif ($onWindows) { $BoostRoot = 'C:/local/boost_1_89_0' }
}
if ($BoostRoot -and (Test-Path $BoostRoot)) {
  Info "BOOST_ROOT=$BoostRoot"
} else {
  if ($BoostRoot) { Warn "BOOST_ROOT 경로가 존재하지 않습니다: $BoostRoot (무시하고 진행)" }
}

# 제너레이터 설정
if (-not $Generator -or $Generator -eq '') {
  if ($onWindows) { $Generator = 'Visual Studio 17 2022' } else { $Generator = 'Unix Makefiles' }
}

$cmakeArgs = @('-S','.', '-B', $BuildDir, '-G', $Generator, "-DCMAKE_BUILD_TYPE=$Config")
if ($onWindows -and $Generator -like 'Visual Studio*') { $cmakeArgs += @('-A','x64') }
if ($BoostRoot -and (Test-Path $BoostRoot)) { $cmakeArgs += @("-DBOOST_ROOT=$BoostRoot") }

# vcpkg toolchain
$vcpkgRoot = $env:VCPKG_ROOT
if (-not $vcpkgRoot -or -not (Test-Path $vcpkgRoot)) {
  # VS 내장 vcpkg 경로 추정
  $vsVcpkg = "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/vcpkg"
  if (Test-Path $vsVcpkg) { $vcpkgRoot = $vsVcpkg }
}

$vcpkgJsonExists = Test-Path 'vcpkg.json'
if ($UseVcpkg -or $vcpkgJsonExists -or ($vcpkgRoot -and (Test-Path $vcpkgRoot))) {
  if (-not $vcpkgRoot -or -not (Test-Path $vcpkgRoot)) { Fail "vcpkg 사용이 요청되었지만 VCPKG_ROOT를 찾을 수 없습니다." }
  Info "VCPKG_ROOT=$vcpkgRoot"
  $toolchain = Join-Path $vcpkgRoot 'scripts/buildsystems/vcpkg.cmake'
  if (-not (Test-Path $toolchain)) { Fail "vcpkg toolchain 파일을 찾지 못했습니다: $toolchain" }
  $cmakeArgs += @("-DCMAKE_TOOLCHAIN_FILE=$toolchain")
  if (-not $VcpkgTriplet -or $VcpkgTriplet -eq '') { if ($onWindows) { $VcpkgTriplet = 'x64-windows' } else { $VcpkgTriplet = 'x64-linux' } }
  $cmakeArgs += @("-DVCPKG_TARGET_TRIPLET=$VcpkgTriplet")
  # CMAKE_PREFIX_PATH에 매니페스트 설치 경로 추가(발견 안정성 강화)
  try {
    $prefix = Join-Path (Resolve-Path .) "vcpkg_installed/$VcpkgTriplet"
    if (Test-Path $prefix) { $cmakeArgs += @("-DCMAKE_PREFIX_PATH=$prefix") }
  } catch {}
  # 매니페스트가 있으면 사전 설치(선택)
  if ($vcpkgJsonExists -and -not $NoVcpkgAutoInstall) {
    $exe = Join-Path $vcpkgRoot $(if ($onWindows) { 'vcpkg.exe' } else { 'vcpkg' })
    if (Test-Path $exe) {
      # builtin-baseline이 없으면 자동 갱신 시도
      $manifest = Get-Content -Raw -ErrorAction SilentlyContinue 'vcpkg.json'
      if ($manifest -and ($manifest -notmatch 'builtin-baseline')) {
        Info "vcpkg builtin-baseline 자동 설정 시도(git 원격 HEAD)"
        $baseline = ''
        try {
          $ls = git ls-remote https://github.com/microsoft/vcpkg HEAD 2>$null
          if ($ls) { $baseline = ($ls -split "\s+")[0] }
        } catch {}
        if ($baseline -and $baseline.Length -ge 8) {
          try {
            $json = Get-Content -Raw 'vcpkg.json' | ConvertFrom-Json
            $json | Add-Member -NotePropertyName 'builtin-baseline' -NotePropertyValue $baseline -Force
            ($json | ConvertTo-Json -Depth 5) | Set-Content -Encoding UTF8 'vcpkg.json'
            Info "builtin-baseline=$baseline 적용"
          } catch {
            Warn "vcpkg.json 수정 실패: $_"
          }
        } else {
          Warn "원격 baseline 해시를 가져오지 못했습니다. 수동으로 vcpkg.json에 'builtin-baseline'을 추가하세요."
        }
      }
      Info "vcpkg 의존성 설치: $VcpkgTriplet"
      & $exe install --triplet $VcpkgTriplet
      if ($LASTEXITCODE -ne 0) { Fail "vcpkg 설치 실패" }
    } else { Warn "vcpkg 실행 파일을 찾지 못했습니다: $exe" }
  }
}

# Windows/MSVC + vcpkg + FTXUI: 일부 패키지가 RelWithDebInfo 매핑을 제공하지 않아 Debug 런타임과의 링크 충돌이 있을 수 있음
if ($onWindows -and ($UseVcpkg -or $vcpkgJsonExists) -and $Generator -like 'Visual Studio*' -and $Config -eq 'RelWithDebInfo') {
  Info "MSVC+vcpkg 환경에서 RelWithDebInfo 대신 Debug 구성으로 빌드(런타임 불일치 회피)"
  $Config = 'Debug'
}

Info "CMake 구성 중..."
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { Fail "CMake 구성 실패" }

Info "빌드 중..."
if (-not $Target -or $Target -eq '') {
  if ($onWindows -and $Generator -like 'Visual Studio*') { $Target = 'ALL_BUILD' } else { $Target = 'all' }
}
& cmake --build $BuildDir --config $Config --target $Target -j
if ($LASTEXITCODE -ne 0) { Fail "빌드 실패" }

if ($InstallPrefix -and $InstallPrefix -ne '') {
  Info "설치 중... ($InstallPrefix)"
  & cmake --install $BuildDir --config $Config --prefix $InstallPrefix
  if ($LASTEXITCODE -ne 0) { Fail "설치 실패" }
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
