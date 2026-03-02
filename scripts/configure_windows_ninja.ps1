<#
  Configure a Ninja build directory on Windows.

  Why this exists:
  - `cmake --preset windows-ninja` needs `ninja.exe`.
  - Many Visual Studio installs bundle Ninja, but it's not always on PATH.
  - Conan toolchain for the preset must be generated before configure.

  This script:
  - Finds Ninja (PATH -> VS bundled -> CMake bundled)
  - Runs `scripts/setup_conan.ps1` for the selected preset configuration
  - Runs `cmake --preset windows-ninja` with `-D CMAKE_MAKE_PROGRAM=...` when needed
  - Optionally copies `build-windows-ninja/compile_commands.json` to repo root for clangd
    (the root file is ignored by git).
 #>

[CmdletBinding()]
param(
  [string]$Preset = "windows-ninja",
  [switch]$CopyCompileCommands
)

$ErrorActionPreference = 'Stop'

function Fail([string]$Message) {
  Write-Host "[fail] $Message" -ForegroundColor Red
  exit 1
}

function Info([string]$Message) {
  Write-Host "[info] $Message" -ForegroundColor Cyan
}

function Resolve-NinjaPath() {
  $cmd = Get-Command ninja -ErrorAction SilentlyContinue
  if ($cmd -and $cmd.Path -and (Test-Path $cmd.Path)) {
    return $cmd.Path
  }

  $vswhereCandidates = @()
  if ($env:ProgramFiles -and $env:ProgramFiles -ne '') {
    $vswhereCandidates += (Join-Path $env:ProgramFiles 'Microsoft Visual Studio\Installer\vswhere.exe')
  }
  try {
    $pf86 = ${env:ProgramFiles(x86)}
    if ($pf86 -and $pf86 -ne '') {
      $vswhereCandidates += (Join-Path $pf86 'Microsoft Visual Studio\Installer\vswhere.exe')
    }
  } catch {}
  $vswhereCandidates += 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'

  $vswhere = $vswhereCandidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
  if ($vswhere) {
    try {
      $installPath = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
      if ($installPath) {
        $installPath = $installPath.Trim()
      }
      if ($installPath -and (Test-Path $installPath)) {
        $vsNinja = Join-Path $installPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
        if (Test-Path $vsNinja) {
          return $vsNinja
        }
      }
    } catch {}
  }

  $cmakeNinja = 'C:\Program Files\CMake\bin\ninja.exe'
  if (Test-Path $cmakeNinja) {
    return $cmakeNinja
  }

  return $null
}

function Resolve-VsWherePath() {
  $candidates = @()
  if ($env:ProgramFiles -and $env:ProgramFiles -ne '') {
    $candidates += (Join-Path $env:ProgramFiles 'Microsoft Visual Studio\Installer\vswhere.exe')
  }
  try {
    $pf86 = ${env:ProgramFiles(x86)}
    if ($pf86 -and $pf86 -ne '') {
      $candidates += (Join-Path $pf86 'Microsoft Visual Studio\Installer\vswhere.exe')
    }
  } catch {}
  $candidates += 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
  return ($candidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1)
}

function Enable-MsvcDevEnvironment() {
  $existing = Get-Command cl -ErrorAction SilentlyContinue
  if ($existing -and $existing.Path) {
    return $true
  }

  $vswhere = Resolve-VsWherePath
  if (-not $vswhere) {
    return $false
  }

  $installPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if (-not $installPath) {
    return $false
  }
  $installPath = $installPath.Trim()
  if (-not (Test-Path $installPath)) {
    return $false
  }

  $vsDevCmd = Join-Path $installPath 'Common7\Tools\VsDevCmd.bat'
  if (-not (Test-Path $vsDevCmd)) {
    return $false
  }

  $devCmd = "`"$vsDevCmd`" -arch=x64 -host_arch=x64 >nul && set"
  $envDump = cmd /s /c $devCmd
  foreach ($line in $envDump) {
    if ($line -match '^(.*?)=(.*)$') {
      [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
  }

  $resolved = Get-Command cl -ErrorAction SilentlyContinue
  return ($null -ne $resolved)
}

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
Push-Location $repoRoot.Path
try {
  $ninja = Resolve-NinjaPath
  if (-not $ninja) {
    Fail "ninja.exe not found. Install Ninja (winget/choco), or install VS CMake tools, then re-run." 
  }

  if (-not (Enable-MsvcDevEnvironment)) {
    Fail "MSVC Developer Command Prompt environment was not initialized (cl.exe not found)."
  }

  Info "Using Ninja: $ninja"
  $conanConfig = 'Debug'
  if ($Preset -match 'release') {
    $conanConfig = 'Release'
  }

  $buildDir = Join-Path $repoRoot.Path 'build-windows-ninja'
  if (Test-Path $buildDir) {
    Info "Resetting build-windows-ninja for clean Conan configure."
    Remove-Item -Path $buildDir -Recurse -Force
  }

  $setupConanScript = Join-Path $repoRoot.Path 'scripts\setup_conan.ps1'
  if (-not (Test-Path $setupConanScript)) {
    Fail "setup_conan.ps1 not found: $setupConanScript"
  }
  Info "Preparing Conan toolchain: config=$conanConfig, buildDir=build-windows-ninja"
  & $setupConanScript -Config $conanConfig -Feature windows-dev -BuildDir build-windows-ninja -ToolchainGenerator Ninja | Out-Null
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

  Info "Configuring with preset: $Preset"

  & cmake --preset $Preset -D "CMAKE_MAKE_PROGRAM=$ninja"
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

  if ($CopyCompileCommands) {
    $src = Join-Path $repoRoot.Path 'build-windows-ninja\compile_commands.json'
    $dst = Join-Path $repoRoot.Path 'compile_commands.json'
    if (Test-Path $src) {
      Copy-Item -Path $src -Destination $dst -Force
      Info "Copied compile_commands.json to repo root (ignored by git)."
    } else {
      Fail "compile_commands.json not found at: $src"
    }
  }
} finally {
  Pop-Location
}
