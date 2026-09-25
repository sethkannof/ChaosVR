param(
  [string]$VcpkgRoot = $env:VCPKG_ROOT,
  [ValidateSet("Debug","Release")][string]$Config = "Release"
)
$ErrorActionPreference = "Stop"
if (-not $VcpkgRoot) { throw "Set VCPKG_ROOT or pass -VcpkgRoot C:\\path\\to\\vcpkg" }
$toolchain = Join-Path $VcpkgRoot "scripts\\buildsystems\\vcpkg.cmake"
if (-not (Test-Path $toolchain)) { throw "vcpkg toolchain not found: $toolchain" }
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $here "build"
cmake -S $here -B $build -A x64 "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
if ($LASTEXITCODE -ne 0) { throw "Host64 CMake configure failed with exit code $LASTEXITCODE" }
cmake --build $build --config $Config
if ($LASTEXITCODE -ne 0) { throw "Host64 build failed with exit code $LASTEXITCODE" }
$exe = Join-Path $build "$Config\\ChaosVRHost64.exe"
if (-not (Test-Path $exe)) { throw "Host64 build reported success but output is missing: $exe" }
Write-Host "Built: $exe"