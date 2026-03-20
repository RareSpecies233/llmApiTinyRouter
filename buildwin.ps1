#!/usr/bin/env pwsh
Param(
  [string]$VcpkgDir = "$PSScriptRoot\vcpkg",
  [string]$BuildDir = "$PSScriptRoot\build_x86",
  [string]$Triplet = "x86-windows",
  [string]$Config = "Release"
)

Set-StrictMode -Version Latest
function Write-Err { Write-Host $args -ForegroundColor Red }

$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Definition
Push-Location $RepoRoot

Write-Host "Repository root: $RepoRoot"

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
  Write-Err "git not found in PATH. Please install Git."
  exit 1
}

# Clone vcpkg if missing
if (-not (Test-Path $VcpkgDir)) {
  Write-Host "Cloning vcpkg..."
  git clone https://github.com/microsoft/vcpkg.git "$VcpkgDir" || { Write-Err "git clone failed"; exit 1 }
}

# Bootstrap vcpkg (creates vcpkg.exe)
$vcpkgExe = Join-Path $VcpkgDir "vcpkg.exe"
if (-not (Test-Path $vcpkgExe)) {
  Write-Host "Bootstrapping vcpkg..."
  & (Join-Path $VcpkgDir "bootstrap-vcpkg.bat") || { Write-Err "vcpkg bootstrap failed"; exit 1 }
}

Write-Host "Installing OpenSSL ($Triplet) via vcpkg..."
& $vcpkgExe install "openssl:$Triplet" || { Write-Err "vcpkg install failed"; exit 1 }

# CMake configure
$toolchain = Join-Path $VcpkgDir "scripts\buildsystems\vcpkg.cmake"
Write-Host "Configuring CMake (Win32 / $Config) ..."
cmake -S $RepoRoot -B $BuildDir -A Win32 -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DVCPKG_TARGET_TRIPLET=$Triplet -DCMAKE_BUILD_TYPE=$Config || { Write-Err "cmake configure failed"; exit 1 }

# Build
Write-Host "Building project..."
cmake --build $BuildDir --config $Config -- /m || { Write-Err "build failed"; exit 1 }

# Copy runtime DLLs from vcpkg installed triplet
$installedBin = Join-Path $VcpkgDir "installed\$Triplet\bin"
$dest = Join-Path $BuildDir $Config
if (Test-Path $installedBin) {
  Write-Host "Copying runtime DLLs from $installedBin to $dest"
  Copy-Item -Path (Join-Path $installedBin "*.dll") -Destination $dest -Force -ErrorAction SilentlyContinue
} else {
  Write-Host "No runtime bin folder found at $installedBin -- skipping DLL copy"
}

# Copy config.example.json if exists
$example = Join-Path $RepoRoot "config.example.json"
if (Test-Path $example) {
  Write-Host "Copying config.example.json to output as config.json"
  Copy-Item -Path $example -Destination (Join-Path $dest "config.json") -Force
}

Write-Host "Build complete. Executable location: $dest\llm_api_proxy.exe"
Pop-Location
