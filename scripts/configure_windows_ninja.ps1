<#
  Configure a Ninja build directory on Windows.

  Why this exists:
  - `cmake --preset windows-ninja` needs `ninja.exe`.
  - Many Visual Studio installs bundle Ninja, but it's not always on PATH.

  This script:
  - Finds Ninja (PATH -> VS bundled -> CMake bundled)
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

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
Push-Location $repoRoot.Path
try {
  $ninja = Resolve-NinjaPath
  if (-not $ninja) {
    Fail "ninja.exe not found. Install Ninja (winget/choco), or install VS CMake tools, then re-run." 
  }

  Info "Using Ninja: $ninja"
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
