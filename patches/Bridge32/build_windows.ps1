param([ValidateSet("Debug","Release")][string]$Config = "Release")
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $here "build"
cmake -S $here -B $build -A Win32
if ($LASTEXITCODE -ne 0) { throw "Bridge32 CMake configure failed with exit code $LASTEXITCODE" }
cmake --build $build --config $Config
if ($LASTEXITCODE -ne 0) { throw "Bridge32 build failed with exit code $LASTEXITCODE" }
$dll = Join-Path $build "$Config\\ChaosVRBridge32.dll"
if (-not (Test-Path $dll)) { throw "Bridge32 build reported success but output is missing: $dll" }
Write-Host "Built: $dll"