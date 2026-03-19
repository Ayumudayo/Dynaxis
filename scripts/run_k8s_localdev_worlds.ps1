param(
    [string]$TopologyConfig = "docker/stack/topologies/default.json",
    [string]$Namespace = "dynaxis-localdev",
    [string]$OutputDir = "build/k8s-localdev",
    [switch]$KubectlValidate = $false
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$ProjectRoot = Resolve-Path (Join-Path $ScriptDir "..")
Set-Location $ProjectRoot

try { chcp 65001 | Out-Null } catch {}

$ManifestPath = Join-Path $OutputDir "worlds-localdev.generated.yaml"
$ActivePath = Join-Path $OutputDir "topology.active.json"
$SummaryPath = Join-Path $OutputDir "summary.json"

function Test-KubectlContextReady {
    param(
        [Parameter(Mandatory = $true)]
        [string]$KubectlPath
    )

    & $KubectlPath config current-context *> $null
    if ($LASTEXITCODE -ne 0) {
        return "current kubectl context is not set"
    }

    & $KubectlPath version --output=json --request-timeout=5s *> $null
    if ($LASTEXITCODE -ne 0) {
        return "current kubectl context is not reachable"
    }

    return $null
}

$Python = Get-Command python -ErrorAction SilentlyContinue
if (-not $Python) {
    $PyLauncher = Get-Command py -ErrorAction SilentlyContinue
    if ($PyLauncher) {
        $PythonExe = $PyLauncher.Source
        $PythonPrefixArgs = @("-3")
    } else {
        throw "Python interpreter not found."
    }
} else {
    $PythonExe = $Python.Source
    $PythonPrefixArgs = @()
}

$Generator = Join-Path $ScriptDir "generate_k8s_topology.py"
$Args = @(
    $Generator,
    "--topology-config", $TopologyConfig,
    "--output-manifest", $ManifestPath,
    "--output-active", $ActivePath,
    "--output-summary", $SummaryPath,
    "--namespace", $Namespace
)

& $PythonExe @PythonPrefixArgs @Args
if ($LASTEXITCODE -ne 0) {
    throw "Failed to generate Kubernetes local/dev worlds manifest."
}

Write-Host "Manifest: $ManifestPath" -ForegroundColor Gray
Write-Host "Summary:  $SummaryPath" -ForegroundColor Gray

if ($KubectlValidate) {
    $Kubectl = Get-Command kubectl -ErrorAction SilentlyContinue
    if (-not $Kubectl) {
        Write-Host "Skipping kubectl validation: kubectl is not installed." -ForegroundColor Yellow
        exit 0
    }

    $KubectlPrerequisite = Test-KubectlContextReady -KubectlPath $Kubectl.Source
    if ($KubectlPrerequisite) {
        Write-Host "Skipping kubectl validation: $KubectlPrerequisite." -ForegroundColor Yellow
        exit 0
    }

    & $Kubectl.Source create --dry-run=client --validate=false -f $ManifestPath -o yaml | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "kubectl client-side validation failed."
    }

    Write-Host "kubectl client-side validation passed." -ForegroundColor Green
}
