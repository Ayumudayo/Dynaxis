param(
    [ValidateSet("up", "status", "down")]
    [string]$Action = "up",
    [string]$ClusterName = "dynaxis-localdev",
    [string]$TopologyConfig = "docker/stack/topologies/default.json",
    [string]$Namespace = "dynaxis-localdev",
    [string]$OutputDir = "build/k8s-localdev",
    [int]$WaitTimeoutSeconds = 240,
    [switch]$CleanNamespace = $false,
    [switch]$RecreateCluster = $false,
    [switch]$SkipImageLoad = $false
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$ProjectRoot = Resolve-Path (Join-Path $ScriptDir "..")
Set-Location $ProjectRoot

try { chcp 65001 | Out-Null } catch {}

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

$Runner = Join-Path $ScriptDir "run_k8s_localdev_kind.py"
$Args = @(
    $Runner,
    $Action,
    "--cluster-name", $ClusterName,
    "--topology-config", $TopologyConfig,
    "--namespace", $Namespace,
    "--output-dir", $OutputDir,
    "--wait-timeout-seconds", $WaitTimeoutSeconds
)

if ($CleanNamespace) {
    $Args += "--clean-namespace"
}
if ($RecreateCluster) {
    $Args += "--recreate-cluster"
}
if ($SkipImageLoad) {
    $Args += "--skip-image-load"
}

& $PythonExe @PythonPrefixArgs @Args
exit $LASTEXITCODE
