param(
    [ValidateSet("Ninja", "NMake")]
    [string]$PreferredGenerator = "Ninja",

    [string]$BuildDir = "build-win32-release",

    [ValidateSet("Release", "Debug", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",

    [switch]$SkipVcpkgInstall,

    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

function Write-Section {
    param([string]$Message)
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Resolve-VsDevCmd {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $installationPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE -eq 0 -and $installationPath) {
            $candidate = Join-Path $installationPath "Common7\Tools\VsDevCmd.bat"
            if (Test-Path $candidate) {
                return $candidate
            }
        }
    }

    $candidates = @(
        "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat",
        "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat",
        "C:\Program Files\Microsoft Visual Studio\18\Professional\Common7\Tools\VsDevCmd.bat",
        "C:\Program Files\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat",
        "C:\Program Files\Microsoft Visual Studio\17\Community\Common7\Tools\VsDevCmd.bat",
        "C:\Program Files\Microsoft Visual Studio\17\Professional\Common7\Tools\VsDevCmd.bat",
        "C:\Program Files\Microsoft Visual Studio\17\BuildTools\Common7\Tools\VsDevCmd.bat"
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    throw "Unable to locate VsDevCmd.bat. Install Visual Studio with Desktop development with C++."
}

function Invoke-InVsDevShell {
    param(
        [string]$VsDevCmd,
        [string]$Command
    )

    $cmdLine = "call `"$VsDevCmd`" -host_arch=x64 -arch=x86 >nul && $Command"
    & cmd.exe /d /s /c $cmdLine
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $Command"
    }
}

function Ensure-Vcpkg {
    param([string]$RepoRoot)

    $toolsDir = Join-Path $RepoRoot ".tools"
    $vcpkgDir = Join-Path $toolsDir "vcpkg"
    $vcpkgExe = Join-Path $vcpkgDir "vcpkg.exe"

    if (-not (Test-Path $toolsDir)) {
        New-Item -ItemType Directory -Path $toolsDir | Out-Null
    }

    if (-not (Test-Path (Join-Path $vcpkgDir ".git"))) {
        Write-Section "Cloning vcpkg"
        git clone --depth 1 https://github.com/microsoft/vcpkg $vcpkgDir
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to clone vcpkg"
        }
    }

    if (-not (Test-Path $vcpkgExe)) {
        Write-Section "Bootstrapping vcpkg"
        & (Join-Path $vcpkgDir "bootstrap-vcpkg.bat") -disableMetrics
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to bootstrap vcpkg"
        }
    }

    return $vcpkgExe
}

function Install-VcpkgDependencies {
    param([string]$VcpkgExe)

    Write-Section "Installing vcpkg dependencies for x86-windows"
    & $VcpkgExe install openssl curl cpp-httplib[openssl] nlohmann-json spdlog --triplet x86-windows
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to install x86-windows dependencies with vcpkg"
    }
}

function Configure-Build {
    param(
        [string]$VsDevCmd,
        [string]$Generator,
        [string]$BinaryDir,
        [string]$ToolchainFile,
        [string]$ConfigName
    )

    if ((Test-Path $BinaryDir) -and $Clean) {
        Write-Section "Removing existing build directory $BinaryDir"
        Remove-Item -Recurse -Force $BinaryDir
    }

    if ($Generator -eq "Ninja") {
        $configureCommand = @(
            "cmake",
            "-S .",
            "-B `"$BinaryDir`"",
            "-G Ninja",
            "-DCMAKE_BUILD_TYPE=$ConfigName",
            "-DCMAKE_TOOLCHAIN_FILE=`"$ToolchainFile`"",
            "-DVCPKG_TARGET_TRIPLET=x86-windows",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
        ) -join " "

        Invoke-InVsDevShell -VsDevCmd $VsDevCmd -Command $configureCommand
        return
    }

    $configureCommand = @(
        "cmake",
        "-S .",
        "-B `"$BinaryDir`"",
        "-G `"NMake Makefiles`"",
        "-DCMAKE_BUILD_TYPE=$ConfigName",
        "-DCMAKE_TOOLCHAIN_FILE=`"$ToolchainFile`"",
        "-DVCPKG_TARGET_TRIPLET=x86-windows",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
    ) -join " "

    Invoke-InVsDevShell -VsDevCmd $VsDevCmd -Command $configureCommand
}

function Build-Project {
    param(
        [string]$VsDevCmd,
        [string]$BinaryDir,
        [string]$ConfigName
    )

    $buildCommand = "cmake --build `"$BinaryDir`" --config $ConfigName --parallel"
    Invoke-InVsDevShell -VsDevCmd $VsDevCmd -Command $buildCommand
}

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $repoRoot

$vsDevCmd = Resolve-VsDevCmd
$vcpkgExe = Ensure-Vcpkg -RepoRoot $repoRoot

if (-not $SkipVcpkgInstall) {
    Install-VcpkgDependencies -VcpkgExe $vcpkgExe
}

$toolchainFile = (Resolve-Path ".\.tools\vcpkg\scripts\buildsystems\vcpkg.cmake").Path
$buildAttempts = @()

if ($PreferredGenerator -eq "Ninja") {
    $buildAttempts += @{ Generator = "Ninja"; BinaryDir = $BuildDir }
    $buildAttempts += @{ Generator = "NMake"; BinaryDir = "$BuildDir-nmake" }
} else {
    $buildAttempts += @{ Generator = "NMake"; BinaryDir = $BuildDir }
    $buildAttempts += @{ Generator = "Ninja"; BinaryDir = "$BuildDir-ninja" }
}

$lastError = $null

foreach ($attempt in $buildAttempts) {
    try {
        Write-Section "Configuring with $($attempt.Generator)"
        Configure-Build -VsDevCmd $vsDevCmd -Generator $attempt.Generator -BinaryDir $attempt.BinaryDir -ToolchainFile $toolchainFile -ConfigName $Configuration

        Write-Section "Building $($attempt.BinaryDir)"
        Build-Project -VsDevCmd $vsDevCmd -BinaryDir $attempt.BinaryDir -ConfigName $Configuration

        $exePath = if ($attempt.Generator -eq "Ninja" -or $attempt.Generator -eq "NMake") {
            Join-Path $repoRoot "$($attempt.BinaryDir)\llm_api_proxy.exe"
        } else {
            Join-Path $repoRoot "$($attempt.BinaryDir)\$Configuration\llm_api_proxy.exe"
        }

        if (-not (Test-Path $exePath)) {
            throw "Build completed but executable was not found at $exePath"
        }

        Write-Section "Build succeeded"
        Write-Host "Executable: $exePath" -ForegroundColor Green
        exit 0
    } catch {
        $lastError = $_
        Write-Warning "Build attempt with $($attempt.Generator) failed: $($_.Exception.Message)"
    }
}

throw "All build attempts failed. Last error: $($lastError.Exception.Message)"
