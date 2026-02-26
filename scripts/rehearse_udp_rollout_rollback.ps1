param(
    [string]$CanaryEnvFile = "docker/stack/.env.udp-canary.example",
    [string]$RollbackEnvFile = "docker/stack/.env.udp-rollback.example",
    [string]$ProjectName = "knights-stack",
    [switch]$Observability = $true,
    [switch]$NoBuild = $false
)

$ErrorActionPreference = "Stop"

try { chcp 65001 | Out-Null } catch {}

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$ProjectRoot = Resolve-Path (Join-Path $ScriptDir "..")
Set-Location $ProjectRoot

$BuildEnabled = -not $NoBuild

function Info([string]$Message) {
    Write-Host "[info] $Message" -ForegroundColor Cyan
}

function Invoke-Deploy([string]$Action, [string]$EnvFile, [switch]$Detached) {
    $deployScript = Join-Path $ScriptDir "deploy_docker.ps1"
    $args = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", $deployScript,
        "-Action", $Action,
        "-ProjectName", $ProjectName
    )

    if ($EnvFile -and $EnvFile.Trim() -ne "") {
        $args += @("-EnvFile", $EnvFile)
    }
    if ($Observability) {
        $args += "-Observability"
    }
    if ($BuildEnabled) {
        $args += "-Build"
    }
    if ($Detached) {
        $args += "-Detached"
    }

    pwsh @args
}

function Wait-EndpointReady([string]$Url, [int]$TimeoutSec = 60) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        try {
            $resp = Invoke-WebRequest -Uri $Url -TimeoutSec 5
            if ($resp.StatusCode -eq 200) {
                return
            }
        } catch {
            Start-Sleep -Milliseconds 500
        }
    }
    throw "Endpoint not ready: $Url"
}

function Get-MetricValueFromEndpoint([string]$MetricsUrl, [string]$MetricName) {
    $py = @"
import sys
import urllib.request

url = sys.argv[1]
metric = sys.argv[2]
text = urllib.request.urlopen(url, timeout=5).read().decode('utf-8', 'replace')
for line in text.splitlines():
    stripped = line.strip()
    if not stripped or stripped.startswith('#'):
        continue
    if not (stripped.startswith(metric + ' ') or stripped.startswith(metric + '{')):
        continue
    parts = stripped.split()
    if len(parts) >= 2:
        print(parts[1])
        raise SystemExit(0)
raise SystemExit(2)
"@

    $raw = python -c $py $MetricsUrl $MetricName
    if ($LASTEXITCODE -ne 0) {
        throw "Metric not found: $MetricName"
    }

    $value = ($raw | Select-Object -First 1).ToString().Trim()
    return [double]::Parse($value, [System.Globalization.CultureInfo]::InvariantCulture)
}

function Assert-GatewayUdpState([string]$MetricsUrl, [double]$ExpectedEnabled) {
    Wait-EndpointReady $MetricsUrl 30
    $deadline = (Get-Date).AddSeconds(30)
    $lastError = ""

    while ((Get-Date) -lt $deadline) {
        try {
            $enabled = Get-MetricValueFromEndpoint $MetricsUrl "gateway_udp_enabled"
            if ($enabled -ne $ExpectedEnabled) {
                $lastError = "gateway_udp_enabled expected=$ExpectedEnabled actual=$enabled"
                Start-Sleep -Milliseconds 500
                continue
            }

            return
        }
        catch {
            $lastError = $_.Exception.Message
            Start-Sleep -Milliseconds 500
        }
    }

    throw "Failed to validate UDP state at $MetricsUrl (last=$lastError)"
}

$sw = [System.Diagnostics.Stopwatch]::StartNew()

Info "Starting canary rollout rehearsal"
Invoke-Deploy -Action "down" -EnvFile $CanaryEnvFile
Invoke-Deploy -Action "up" -EnvFile $CanaryEnvFile -Detached

Info "Running TCP smoke on canary stage"
python tests/python/verify_pong.py

Assert-GatewayUdpState "http://127.0.0.1:36001/metrics" 1
Assert-GatewayUdpState "http://127.0.0.1:36002/metrics" 0

Info "Executing rollback to TCP-only"
Invoke-Deploy -Action "down" -EnvFile $CanaryEnvFile
Invoke-Deploy -Action "up" -EnvFile $RollbackEnvFile -Detached

Info "Running TCP smoke after rollback"
python tests/python/verify_pong.py

Assert-GatewayUdpState "http://127.0.0.1:36001/metrics" 0
Assert-GatewayUdpState "http://127.0.0.1:36002/metrics" 0

$sw.Stop()
$elapsedSec = [math]::Round($sw.Elapsed.TotalSeconds, 2)

if ($sw.Elapsed.TotalMinutes -gt 10) {
    throw "Rollback rehearsal exceeded 10 minutes ($elapsedSec sec)"
}

Info "Rollback rehearsal completed in $elapsedSec sec (<= 10 min)"
