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
  [switch]$Clean,
  [string]$InstallPrefix = "",
  [ValidateSet('none','server','client')]
  [string]$Run = 'none',
[int]$Port = 5000
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

if ($Clean) {
  if (Test-Path $BuildDir) { Info "빌드 폴더 정리: $BuildDir"; Remove-Item -Recurse -Force $BuildDir }
}

# Boost 경로 설정
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

Info "CMake 구성 중..."
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { Fail "CMake 구성 실패" }

Info "빌드 중..."
& cmake --build $BuildDir --config $Config -j
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
}

Info "완료"
