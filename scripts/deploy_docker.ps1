# deploy_docker.ps1
param (
    [string]$Action = "up", # up, down, restart, build, logs, ps, clean, config
    [switch]$Detached = $false,
    [switch]$Build = $false,
    [switch]$NoCache = $false,
    [switch]$Observability = $false,
    [string]$ProjectName = "",
    [switch]$NoBase = $false
)

$ErrorActionPreference = "Stop"

# 스크립트 위치를 기준으로 프로젝트 루트(상위 디렉토리) 경로 설정
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$ProjectRoot = Resolve-Path (Join-Path $ScriptDir "..")
Set-Location $ProjectRoot
Write-Host "Working Directory set to: $ProjectRoot" -ForegroundColor Gray

# UTF-8 콘솔 설정
try { chcp 65001 | Out-Null } catch {}

function Test-Docker {
    try {
        docker --version | Out-Null
        docker compose version | Out-Null
    }
    catch {
        Write-Error "Docker Desktop or Docker Compose is not installed or not in PATH."
    }
}

function Resolve-ComposeTarget {
    $resolved = Resolve-Path "docker/stack/docker-compose.yml" -ErrorAction Stop
    $composePath = $resolved.Path
    $composeDir = Split-Path -Parent $composePath

    if (-not $ProjectName -or $ProjectName.Trim() -eq "") {
        $ProjectName = "knights-stack"
    }

    return @{
        ComposePath = $composePath
        ComposeDir = $composeDir
        ProjectName = $ProjectName
    }
}

function Maybe-PrintComposeEnvHint([string]$ComposeDir) {
    $envPath = Join-Path $ComposeDir ".env"
    if (-not (Test-Path $envPath)) {
        Write-Host "No compose .env found: $envPath (using defaults)" -ForegroundColor Gray
    }
}

function Needs-BaseImage([string]$ComposePath) {
    return (-not $NoBase)
}

function Ensure-BaseImage {
    if ($NoCache -or -not (docker images -q knights-base)) {
        Write-Host "Building base image 'knights-base'..." -ForegroundColor Yellow
        $BuildArgs = @("build", "-f", "Dockerfile.base", "-t", "knights-base", ".")
        if ($NoCache) { $BuildArgs += "--no-cache" }
        docker @BuildArgs
        if ($LASTEXITCODE -ne 0) {
            Write-Error "Failed to build base image 'knights-base'."
        }
    }
}

Test-Docker

$target = Resolve-ComposeTarget
$ComposePath = $target.ComposePath
$ComposeDir = $target.ComposeDir
$ProjectName = $target.ProjectName

Write-Host "Compose: $ComposePath" -ForegroundColor Gray
Write-Host "Project: $ProjectName" -ForegroundColor Gray
Maybe-PrintComposeEnvHint $ComposeDir

$ComposeBaseArgs = @(
    "compose",
    "--project-name", $ProjectName,
    "--project-directory", $ComposeDir,
    "-f", $ComposePath
)

if ($Observability) {
    $ComposeBaseArgs += @("--profile", "observability")
}

if ($Action -eq "build") {
    if (Needs-BaseImage $ComposePath) {
        Ensure-BaseImage
    }

    Write-Host "Building Docker images..." -ForegroundColor Cyan
    $ComposeArgs = $ComposeBaseArgs + @("build")
    if ($NoCache) { $ComposeArgs += "--no-cache" }
    docker @ComposeArgs
}
elseif ($Action -eq "up") {
    if (Needs-BaseImage $ComposePath) {
        # Up 실행 시에도 Base Image가 없으면 빌드해야 함 (Build 옵션이 켜져있거나 이미지가 없을 때)
        if ($Build -or $NoCache -or -not (docker images -q knights-base)) {
            Ensure-BaseImage
        }
    }

    Write-Host "Starting services..." -ForegroundColor Cyan
    $DockerArgs = $ComposeBaseArgs + @("up")
    if ($Detached) { $DockerArgs += "-d" }
    if ($Build) { $DockerArgs += "--build" }
    docker @DockerArgs
}
elseif ($Action -eq "down") {
    Write-Host "Stopping services..." -ForegroundColor Cyan
    docker @($ComposeBaseArgs + @("down"))
}
elseif ($Action -eq "restart") {
    Write-Host "Restarting services..." -ForegroundColor Cyan
    docker @($ComposeBaseArgs + @("restart"))
}
elseif ($Action -eq "logs") {
    docker @($ComposeBaseArgs + @("logs", "-f"))
}
elseif ($Action -eq "ps") {
    docker @($ComposeBaseArgs + @("ps"))
}
elseif ($Action -eq "clean") {
    Write-Host "Stopping and removing services, networks, and volumes..." -ForegroundColor Cyan
    docker @($ComposeBaseArgs + @("down", "-v"))
}
elseif ($Action -eq "config") {
    docker @($ComposeBaseArgs + @("config", "--quiet"))
}
else {
    Write-Error "Unknown action: $Action. Use 'up', 'down', 'restart', 'build', 'logs', 'ps', 'clean', or 'config'."
}
