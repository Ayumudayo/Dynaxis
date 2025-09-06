<#
  Windows에서 Boost.System을 빌드/준비하는 스크립트
  - C:\local\boost_1_89_0 에서 bootstrap.bat 및 b2 실행
  - stage\lib에 .lib 생성 후 BOOST_ROOT/BOOST_LIBRARYDIR 환경변수 설정
#>
[CmdletBinding()]
param(
  [string]$BoostRoot = 'C:/local/boost_1_89_0',
  [string]$Toolset = 'msvc-14.3',
  [ValidateSet('32','64')][string]$AddressModel = '64',
  [string]$Link = 'static,shared'
)

$ErrorActionPreference = 'Stop'
function Info($m){ Write-Host "[info] $m" -ForegroundColor Cyan }
function Fail($m){ Write-Host "[fail] $m" -ForegroundColor Red; exit 1 }

if (-not (Test-Path $BoostRoot)) { Fail "경로 없음: $BoostRoot" }
Push-Location $BoostRoot
try {
  if (-not (Test-Path (Join-Path $BoostRoot 'bootstrap.bat'))) { Fail 'bootstrap.bat 없음' }
  Info 'bootstrap'
  cmd /c "bootstrap.bat"
  Info 'b2 build (system)'
  & .\b2 --with-system variant=release address-model=$AddressModel toolset=$Toolset link=$Link stage
  if ($LASTEXITCODE -ne 0) { Fail 'b2 빌드 실패' }
  $stage = Join-Path $BoostRoot 'stage\lib'
  if (-not (Test-Path $stage)) { Fail 'stage\\lib 없음(빌드 실패)' }
  Info "환경변수 설정: BOOST_ROOT=$BoostRoot, BOOST_LIBRARYDIR=$stage"
  [Environment]::SetEnvironmentVariable('BOOST_ROOT', $BoostRoot, 'User')
  [Environment]::SetEnvironmentVariable('BOOST_LIBRARYDIR', $stage, 'User')
  $env:BOOST_ROOT = $BoostRoot
  $env:BOOST_LIBRARYDIR = $stage
  Info '완료'
} finally {
  Pop-Location
}

