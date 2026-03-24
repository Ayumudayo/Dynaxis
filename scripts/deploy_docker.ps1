# deploy_docker.ps1
param (
    [string]$Action = "up", # up, down, restart, build, logs, ps, clean, config
    [switch]$Detached = $false,
    [switch]$Build = $false,
    [switch]$NoCache = $false,
    [switch]$Observability = $false,
    [string]$ProjectName = "",
    [switch]$NoBase = $false,
    [string]$EnvFile = "",
    [string]$TopologyConfig = ""
)

$ErrorActionPreference = "Stop"

# 스크립트 위치를 기준으로 프로젝트 루트(상위 디렉토리) 경로 설정
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$ProjectRoot = Resolve-Path (Join-Path $ScriptDir "..")
Set-Location $ProjectRoot
Write-Host "Working Directory set to: $ProjectRoot" -ForegroundColor Gray

# UTF-8 콘솔 설정
try { chcp 65001 | Out-Null } catch {}

function Invoke-DockerCommand {
    param (
        [Parameter(Mandatory = $true)]
        [string[]]$Args,
        [string]$FailureMessage = "Docker command failed."
    )

    & docker @Args
    if ($LASTEXITCODE -ne 0) {
        Write-Error "$FailureMessage (exit code: $LASTEXITCODE)"
    }
}

function Test-DockerImageExists([string]$ImageName) {
    $imageId = docker images -q $ImageName 2>$null
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Failed to query Docker image '$ImageName'."
    }
    return -not [string]::IsNullOrWhiteSpace($imageId)
}

function Test-Docker {
    try {
        Invoke-DockerCommand -Args @("--version") -FailureMessage "Docker is not available"
        Invoke-DockerCommand -Args @("compose", "version") -FailureMessage "Docker Compose is not available"
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
        $ProjectName = "dynaxis-stack"
    }

    return @{
        ComposePath = $composePath
        ComposeDir = $composeDir
        ProjectName = $ProjectName
    }
}

function Resolve-PythonCommand {
    $python = Get-Command python -ErrorAction SilentlyContinue
    if ($python) {
        return @{
            Executable = $python.Source
            Arguments = @()
        }
    }

    $py = Get-Command py -ErrorAction SilentlyContinue
    if ($py) {
        return @{
            Executable = $py.Source
            Arguments = @("-3")
        }
    }

    Write-Error "Python interpreter not found. Install python or py launcher."
}

function Resolve-ComposeEnvFile([string]$EnvFilePath, [string]$ComposeDir, [string]$ProjectRoot) {
    if (-not $EnvFilePath -or $EnvFilePath.Trim() -eq "") {
        return $null
    }

    if ([System.IO.Path]::IsPathRooted($EnvFilePath)) {
        if (-not (Test-Path $EnvFilePath)) {
            Write-Error "Compose env file not found: $EnvFilePath"
        }
        return (Resolve-Path $EnvFilePath).Path
    }

    $projectCandidate = Join-Path $ProjectRoot $EnvFilePath
    if (Test-Path $projectCandidate) {
        return (Resolve-Path $projectCandidate).Path
    }

    $composeCandidate = Join-Path $ComposeDir $EnvFilePath
    if (Test-Path $composeCandidate) {
        return (Resolve-Path $composeCandidate).Path
    }

    Write-Error "Compose env file not found (checked project and compose dir): $EnvFilePath"
}

function Maybe-PrintComposeEnvHint([string]$ComposeDir, [string]$ResolvedEnvFile) {
    if ($ResolvedEnvFile -and $ResolvedEnvFile.Trim() -ne "") {
        Write-Host "Using compose env file: $ResolvedEnvFile" -ForegroundColor Gray
        return
    }

    $defaultEnvPath = Join-Path $ComposeDir ".env"
    if (Test-Path $defaultEnvPath) {
        Write-Host "Using default compose env file: $defaultEnvPath" -ForegroundColor Gray
    } else {
        Write-Host "No compose .env found: $defaultEnvPath (using defaults)" -ForegroundColor Gray
    }
}

function Resolve-TopologyConfigPath([string]$TopologyConfigValue,
                                    [string]$ComposeDir,
                                    [string]$ProjectRoot,
                                    [string]$ActionName) {
    if ($TopologyConfigValue -and $TopologyConfigValue.Trim() -ne "") {
        if ([System.IO.Path]::IsPathRooted($TopologyConfigValue)) {
            if (-not (Test-Path $TopologyConfigValue)) {
                Write-Error "Topology config not found: $TopologyConfigValue"
            }
            return (Resolve-Path $TopologyConfigValue).Path
        }

        $projectCandidate = Join-Path $ProjectRoot $TopologyConfigValue
        if (Test-Path $projectCandidate) {
            return (Resolve-Path $projectCandidate).Path
        }

        $composeCandidate = Join-Path $ComposeDir $TopologyConfigValue
        if (Test-Path $composeCandidate) {
            return (Resolve-Path $composeCandidate).Path
        }

        Write-Error "Topology config not found (checked project and compose dir): $TopologyConfigValue"
    }

    $activeTopology = Join-Path $ComposeDir "topology.active.json"
    if ($ActionName -in @("down", "restart", "logs", "ps", "clean") -and (Test-Path $activeTopology)) {
        return (Resolve-Path $activeTopology).Path
    }

    return (Resolve-Path (Join-Path $ComposeDir "topologies/default.json")).Path
}

function Generate-StackTopology([string]$TopologyConfigPath,
                                [string]$ComposeDir,
                                [string]$ProjectRoot) {
    $pythonCommand = Resolve-PythonCommand
    $generatorScript = Join-Path $ProjectRoot "scripts/generate_stack_topology.py"
    if (-not (Test-Path $generatorScript)) {
        Write-Error "Missing topology generator: $generatorScript"
    }

    $generatedCompose = Join-Path $ComposeDir "docker-compose.topology.generated.yml"
    $activeTopology = Join-Path $ComposeDir "topology.active.json"
    Write-Host "Using topology config: $TopologyConfigPath" -ForegroundColor Gray

    $args = @(
        $generatorScript,
        "--topology-config", $TopologyConfigPath,
        "--output-compose", $generatedCompose,
        "--output-active", $activeTopology
    )

    & $pythonCommand.Executable @($pythonCommand.Arguments) @args
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Failed to generate stack topology compose."
    }

    return @{
        GeneratedCompose = $generatedCompose
        ActiveTopology = $activeTopology
    }
}

function Test-ComposeConfiguration([string[]]$ComposeArgs) {
    Invoke-DockerCommand -Args ($ComposeArgs + @("config", "--quiet")) -FailureMessage "Docker Compose config validation failed."
}

function Needs-BaseImage([string]$ComposePath) {
    return (-not $NoBase)
}

function Ensure-BaseImage {
    if ($NoCache -or -not (Test-DockerImageExists "dynaxis-base")) {
        Write-Host "Building base image 'dynaxis-base'..." -ForegroundColor Yellow
        $BuildArgs = @("build", "-f", "Dockerfile.base", "-t", "dynaxis-base", ".")
        if ($NoCache) { $BuildArgs += "--no-cache" }
        Invoke-DockerCommand -Args $BuildArgs -FailureMessage "Failed to build base image 'dynaxis-base'."
    }
}

function Get-RuntimeImageBuildSpecs([string]$ProjectRoot) {
    $dockerfilePath = Join-Path $ProjectRoot "Dockerfile"
    return @(
        @{
            Name = "server-runtime"
            Image = "dynaxis-server:local"
            Target = "server-runtime"
            Dockerfile = $dockerfilePath
            Context = $ProjectRoot
        },
        @{
            Name = "gateway-runtime"
            Image = "dynaxis-gateway:local"
            Target = "gateway-runtime"
            Dockerfile = $dockerfilePath
            Context = $ProjectRoot
        },
        @{
            Name = "worker-runtime"
            Image = "dynaxis-worker:local"
            Target = "worker-runtime"
            Dockerfile = $dockerfilePath
            Context = $ProjectRoot
        },
        @{
            Name = "admin-runtime"
            Image = "dynaxis-admin:local"
            Target = "admin-runtime"
            Dockerfile = $dockerfilePath
            Context = $ProjectRoot
        },
        @{
            Name = "migrator-runtime"
            Image = "dynaxis-migrator:local"
            Target = "migrator-runtime"
            Dockerfile = $dockerfilePath
            Context = $ProjectRoot
        }
    )
}

function Build-RuntimeImages([string]$ProjectRoot,
                             [switch]$NoCache) {
    $specs = Get-RuntimeImageBuildSpecs -ProjectRoot $ProjectRoot
    foreach ($spec in $specs) {
        Write-Host "Building image '$($spec.Image)' (target '$($spec.Target)')..." -ForegroundColor Yellow
        $buildArgs = @(
            "build",
            "-f", $spec.Dockerfile,
            "--target", $spec.Target,
            "-t", $spec.Image
        )
        if ($NoCache) { $buildArgs += "--no-cache" }
        $buildArgs += $spec.Context
        Invoke-DockerCommand -Args $buildArgs -FailureMessage "Failed to build image '$($spec.Image)'."
    }
}

Test-Docker

$target = Resolve-ComposeTarget
$ComposePath = $target.ComposePath
$ComposeDir = $target.ComposeDir
$ProjectName = $target.ProjectName

$ResolvedEnvFile = Resolve-ComposeEnvFile -EnvFilePath $EnvFile -ComposeDir $ComposeDir -ProjectRoot $ProjectRoot
$ResolvedTopologyConfig = Resolve-TopologyConfigPath -TopologyConfigValue $TopologyConfig -ComposeDir $ComposeDir -ProjectRoot $ProjectRoot -ActionName $Action
$TopologyArtifacts = Generate-StackTopology -TopologyConfigPath $ResolvedTopologyConfig -ComposeDir $ComposeDir -ProjectRoot $ProjectRoot

Write-Host "Compose: $ComposePath" -ForegroundColor Gray
Write-Host "Project: $ProjectName" -ForegroundColor Gray
Maybe-PrintComposeEnvHint $ComposeDir $ResolvedEnvFile

$ComposeBaseArgs = @(
    "compose",
    "--project-name", $ProjectName,
    "--project-directory", $ComposeDir,
    "-f", $ComposePath,
    "-f", $TopologyArtifacts.GeneratedCompose
)

if ($ResolvedEnvFile) {
    $ComposeBaseArgs += @("--env-file", $ResolvedEnvFile)
}

if ($Observability) {
    $ComposeBaseArgs += @("--profile", "observability")
}

if ($Action -eq "build") {
    Test-ComposeConfiguration $ComposeBaseArgs
    if (Needs-BaseImage $ComposePath) {
        Ensure-BaseImage
    }

    Write-Host "Building Docker images..." -ForegroundColor Cyan
    Build-RuntimeImages -ProjectRoot $ProjectRoot -NoCache:$NoCache
}
elseif ($Action -eq "up") {
    Test-ComposeConfiguration $ComposeBaseArgs
    if (Needs-BaseImage $ComposePath) {
        # Up 실행 시에도 Base Image가 없으면 빌드해야 함 (Build 옵션이 켜져있거나 이미지가 없을 때)
        if ($Build -or $NoCache -or -not (Test-DockerImageExists "dynaxis-base")) {
            Ensure-BaseImage
        }
    }

    if ($Build) {
        Build-RuntimeImages -ProjectRoot $ProjectRoot -NoCache:$NoCache
    }

    Write-Host "Starting services..." -ForegroundColor Cyan
    $DockerArgs = $ComposeBaseArgs + @("up")
    if ($Detached) { $DockerArgs += "-d" }
    $DockerArgs += "--remove-orphans"
    Invoke-DockerCommand -Args $DockerArgs -FailureMessage "Docker Compose up failed."
}
elseif ($Action -eq "down") {
    Write-Host "Stopping services..." -ForegroundColor Cyan
    Invoke-DockerCommand -Args ($ComposeBaseArgs + @("down")) -FailureMessage "Docker Compose down failed."
}
elseif ($Action -eq "restart") {
    Write-Host "Restarting services..." -ForegroundColor Cyan
    Invoke-DockerCommand -Args ($ComposeBaseArgs + @("restart")) -FailureMessage "Docker Compose restart failed."
}
elseif ($Action -eq "logs") {
    Invoke-DockerCommand -Args ($ComposeBaseArgs + @("logs", "-f")) -FailureMessage "Docker Compose logs failed."
}
elseif ($Action -eq "ps") {
    Invoke-DockerCommand -Args ($ComposeBaseArgs + @("ps")) -FailureMessage "Docker Compose ps failed."
}
elseif ($Action -eq "clean") {
    Write-Host "Stopping and removing services, networks, and volumes..." -ForegroundColor Cyan
    Invoke-DockerCommand -Args ($ComposeBaseArgs + @("down", "-v")) -FailureMessage "Docker Compose clean failed."
}
elseif ($Action -eq "config") {
    Test-ComposeConfiguration $ComposeBaseArgs
}
else {
    Write-Error "Unknown action: $Action. Use 'up', 'down', 'restart', 'build', 'logs', 'ps', 'clean', or 'config'."
}
