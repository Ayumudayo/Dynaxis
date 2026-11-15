param(
    [string]$Image = "dynaxis-base:latest",
    [string]$BuildDir = "build-linux-package-smoke",
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildDirLinux = $BuildDir.Replace('\', '/')
$configValue = $Config

$containerCommand = "python scripts/build.py --profile server --config $Config --build-dir $buildDirLinux --step all && ctest --test-dir $buildDirLinux -R CoreInstalledPackageConsumer --output-on-failure --no-tests=error"
docker run --rm -v "${repoRoot}:/app" -v dynaxis-package-conan:/root/.conan2 -w /app $Image bash -lc $containerCommand
exit $LASTEXITCODE
