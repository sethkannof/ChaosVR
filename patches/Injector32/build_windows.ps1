param([ValidateSet("Debug","Release")][string]$Config = "Release")
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $here "build"
cmake -S $here -B $build -A Win32
if ($LASTEXITCODE -ne 0) { throw "Injector32 CMake configure failed with exit code $LASTEXITCODE" }
cmake --build $build --config $Config
if ($LASTEXITCODE -ne 0) { throw "Injector32 build failed with exit code $LASTEXITCODE" }
$exe = Join-Path $build "$Config\\ChaosVRInjector32.exe"
if (-not (Test-Path $exe)) { throw "Injector32 build reported success but output is missing: $exe" }
Write-Host "Built: $exe"