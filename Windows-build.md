# Windows Build Guide

## Goal

Build a Win32 Release executable for this project on Windows, preferring the Ninja + vcpkg path and falling back to NMake if Ninja cannot complete.

## What This Project Needs

This project already vendors cpp-httplib, nlohmann/json, and spdlog through CMake FetchContent.

The blocking dependencies on Windows are still system packages:

- OpenSSL
- libcurl

Without a package manager or explicit paths, CMake cannot satisfy these calls:

```cmake
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
```

That is why a plain Win32 configure fails unless a Windows package source is attached.

## Recommended Path

Use the repository-local script:

```powershell
.\buildWindows.ps1
```

The script does the following:

1. Locates a Visual Studio C++ toolchain.
2. Clones and bootstraps vcpkg into .tools/vcpkg if it is not already present.
3. Installs curl and openssl for the x86-windows triplet.
4. Installs cpp-httplib, nlohmann-json, and spdlog from vcpkg so Windows does not have to configure those projects from source.
5. Configures a Win32 Release build with Ninja.
6. Builds the executable.
7. Falls back to NMake if the Ninja path fails.

## Output Locations

If Ninja succeeds, the executable is expected at:

```text
build-win32-release/llm_api_proxy.exe
```

If the NMake fallback is used, the executable is expected at:

```text
build-win32-release-nmake/llm_api_proxy.exe
```

## Manual Build Steps

If you want to run the steps yourself instead of using the script:

1. Clone and bootstrap vcpkg.
2. Install the x86 packages.
3. Open a Visual Studio x86 developer shell.
4. Configure with Ninja and the vcpkg toolchain.
5. Build in Release mode.

Example flow:

```powershell
git clone --depth 1 https://github.com/microsoft/vcpkg .tools/vcpkg
.\.tools\vcpkg\bootstrap-vcpkg.bat -disableMetrics
.\.tools\vcpkg\vcpkg.exe install openssl curl cpp-httplib[openssl] nlohmann-json spdlog --triplet x86-windows
```

Then, from a Visual Studio developer command shell configured for x86:

```powershell
cmake -S . -B build-win32-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=.tools/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x86-windows
cmake --build build-win32-release --parallel
```

## Troubleshooting

### CMake cannot find CURL or OpenSSL

Cause:

- The configure step is not using the vcpkg toolchain.
- The packages were installed for the wrong triplet.

Fix:

- Make sure the configure command includes the vcpkg toolchain file.
- Make sure the packages were installed for x86-windows, not x64-windows.

### Ninja configure fails with compiler or environment errors

Cause:

- Ninja with MSVC requires a properly initialized Visual Studio build environment.

Fix:

- Run from VsDevCmd with x86 selected.
- Or let buildWindows.ps1 fall back to NMake.

### Existing build directory is pinned to a different architecture

Cause:

- CMake caches generator, compiler, architecture, and package paths per build directory.

Fix:

- Use a dedicated build directory for Win32 Release.
- Or rerun the script with -Clean.

## Postmortem

The main build issue on this repository is not the project source itself. The failure point is dependency resolution on Windows.

Key observations:

1. The original build cache already targeted Win32 with Ninja, but it was a Debug cache and had unresolved CURL and OpenSSL entries.
2. The machine had Ninja installed, but vcpkg was not available on PATH.
3. A partial local vcpkg cache existed under the user profile, but not a usable project-local vcpkg checkout.
4. The original FetchContent path for cpp-httplib is fragile on Windows because that subproject performs its own dependency checks during configure.
5. The most reliable path was to vendor a local vcpkg instance in the repository, install x86-windows packages for all required third-party libraries, and let the top-level CMake project consume those packages directly.
6. For repeatability on Windows, a fallback generator is worth keeping because Ninja depends on a correct MSVC environment.

## Verified Command Entry Point

The preferred repeatable entry point for this repository is:

```powershell
.\buildWindows.ps1 -PreferredGenerator Ninja -Configuration Release
```