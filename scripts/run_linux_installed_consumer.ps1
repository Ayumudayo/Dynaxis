param(
    [string]$Image = "dynaxis-base:latest",
    [string]$BuildDir = "build-linux-package-smoke",
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildDirLinux = $BuildDir.Replace('\', '/')
$configValue = $Config

$containerCommand = @(
    "set -euo pipefail"
    "cd /workspace"
    "cmake -S . -B $buildDirLinux -G Ninja -DCMAKE_BUILD_TYPE=$configValue -DBUILD_GTEST_TESTS=OFF -DBUILD_SERVER_STACK=OFF -DBUILD_GATEWAY_APP=OFF -DBUILD_WRITE_BEHIND_TOOLS=OFF -DBUILD_LOADGEN_TOOLS=OFF -DBUILD_ADMIN_APP=OFF -DBUILD_MIGRATIONS_RUNNER=OFF"
    "cmake --build $buildDirLinux --target server_core core_public_api_smoke core_public_api_headers_compile core_public_api_stable_header_scenarios --parallel"
    "ctest --test-dir $buildDirLinux -R ""CoreInstalledPackageConsumer|CoreApiBoundaryFixtures|CoreApiStableGovernanceFixtures"" --output-on-failure"
) -join "`n"

docker run --rm `
    -v "${repoRoot}:/workspace" `
    -w /workspace `
    $Image `
    bash -lc $containerCommand
