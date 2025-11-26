# deploy_docker.ps1
param (
    [string]$Action = "up", # up, down, restart, build, logs, ps, clean
    [switch]$Detached = $false,
    [switch]$Build = $false,
    [switch]$NoCache = $false
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

function Test-Env {
    if (-not (Test-Path ".env")) {
        Write-Warning ".env file not found. Copying .env.example to .env..."
        if (Test-Path ".env.example") {
            Copy-Item ".env.example" ".env"
            Write-Host ".env created from .env.example. Please review configuration." -ForegroundColor Yellow
        }
        else {
            Write-Error ".env.example not found. Cannot create .env."
        }
    }
}

Test-Docker
Test-Env

if ($Action -eq "build") {
    # 1. Base Image 확인 및 빌드
    # NoCache가 켜져있거나 이미지가 없으면 빌드
    if ($NoCache -or -not (docker images -q knights-base)) {
        Write-Host "Building base image 'knights-base'..." -ForegroundColor Yellow
        $BuildArgs = @("build", "-f", "Dockerfile.base", "-t", "knights-base", ".")
        if ($NoCache) { $BuildArgs += "--no-cache" }
        docker @BuildArgs
        if ($LASTEXITCODE -ne 0) {
            Write-Error "Failed to build base image 'knights-base'."
        }
    }
    else {
        Write-Host "Base image 'knights-base' found." -ForegroundColor Green
    }

    # 2. Application Image 빌드
    Write-Host "Building Application Docker images..." -ForegroundColor Cyan
    $ComposeArgs = @("compose", "build")
    if ($NoCache) { $ComposeArgs += "--no-cache" }
    docker @ComposeArgs
}
elseif ($Action -eq "up") {
    # Up 실행 시에도 Base Image가 없으면 빌드해야 함 (Build 옵션이 켜져있거나 이미지가 없을 때)
    if ($Build -or $NoCache -or -not (docker images -q knights-base)) {
        if ($NoCache -or -not (docker images -q knights-base)) {
            Write-Host "Building base image 'knights-base'..." -ForegroundColor Yellow
            $BuildArgs = @("build", "-f", "Dockerfile.base", "-t", "knights-base", ".")
            if ($NoCache) { $BuildArgs += "--no-cache" }
            docker @BuildArgs
        }
    }

    Write-Host "Starting services..." -ForegroundColor Cyan
    $DockerArgs = @("compose", "up")
    if ($Detached) { $DockerArgs += "-d" }
    if ($Build) { $DockerArgs += "--build" }
    docker @DockerArgs
}
elseif ($Action -eq "down") {
    Write-Host "Stopping services..." -ForegroundColor Cyan
    docker compose down
}
elseif ($Action -eq "restart") {
    Write-Host "Restarting services..." -ForegroundColor Cyan
    docker compose restart
}
elseif ($Action -eq "logs") {
    docker compose logs -f
}
elseif ($Action -eq "ps") {
    docker compose ps
}
elseif ($Action -eq "clean") {
    Write-Host "Stopping and removing services, networks, and volumes..." -ForegroundColor Cyan
    docker compose down -v
}
else {
    Write-Error "Unknown action: $Action. Use 'up', 'down', 'restart', 'build', 'logs', 'ps', or 'clean'."
}
