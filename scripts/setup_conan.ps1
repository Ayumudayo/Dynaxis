# Compatibility entry point; the shared Python build owns dependency preparation.
[CmdletBinding()]
param(
  [ValidateSet('Debug','Release','RelWithDebInfo','MinSizeRel')][string]$Config = 'Debug',
  [ValidateSet('windows-dev','windows-client','server','core')][string]$Feature = 'windows-dev',
  [string]$BuildDir = '', [string]$ToolchainGenerator = '', [switch]$SkipInstall
)
$ErrorActionPreference = 'Stop'
$profileName = @{ 'windows-dev'='full'; 'windows-client'='client'; 'server'='server'; 'core'='core' }[$Feature]
$commonArgs = @((Join-Path $PSScriptRoot 'build.py'), '--step', 'deps', '--profile', $profileName, '--config', $Config)
if ($BuildDir) { $commonArgs += @('--build-dir', $BuildDir) }
if ($ToolchainGenerator) { $commonArgs += @('--generator', $ToolchainGenerator) }
if (-not $SkipInstall) {
  & python @commonArgs | Out-Host
  if ($LASTEXITCODE -ne 0) { throw 'Dependency preparation failed' }
}
if (-not $BuildDir) {
  $native = if ($IsWindows) { 'windows' } elseif ($IsMacOS) { 'macos' } else { 'linux' }
  $BuildDir = "build/$native/$profileName/$($Config.ToLowerInvariant())"
}
Write-Output (Join-Path (Join-Path $PSScriptRoot '..') "$BuildDir/conan")
