param(
  [ValidateSet("Debug","Release")][string]$Config = "Release",
  [string]$VcpkgRoot = $env:VCPKG_ROOT
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

& (Join-Path $root "Host64\build_windows.ps1") -Config $Config -VcpkgRoot $VcpkgRoot
& (Join-Path $root "Bridge32\build_windows.ps1") -Config $Config
& (Join-Path $root "Injector32\build_windows.ps1") -Config $Config

$out = Join-Path $root "dist\ChaosVR-Community-PreAlpha"
$zip = Join-Path $root "dist\ChaosVR-Community-PreAlpha.zip"
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item $zip -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $out "Bin") | Out-Null

Copy-Item (Join-Path $root "Host64\build\$Config\ChaosVRHost64.exe") (Join-Path $out "Bin")

# Host64 links to the Khronos OpenXR loader dynamically. vcpkg manifest mode
# installs the runtime DLL under the Host64 build tree; bundle it app-local so
# testers do not need a developer/vcpkg environment or a PATH entry.
$openXrLoaderCandidates = @(
  (Join-Path $root "Host64\build\vcpkg_installed\x64-windows\bin\openxr_loader.dll"),
  (Join-Path $VcpkgRoot "installed\x64-windows\bin\openxr_loader.dll")
)
$openXrLoader = $openXrLoaderCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $openXrLoader) {
  throw "openxr_loader.dll was not found after Host64 build. Checked: $($openXrLoaderCandidates -join ', ')"
}
Copy-Item $openXrLoader (Join-Path $out "Bin\openxr_loader.dll")
Copy-Item (Join-Path $root "Bridge32\build\$Config\ChaosVRBridge32.dll") (Join-Path $out "Bin")
Copy-Item (Join-Path $root "Injector32\build\$Config\ChaosVRInjector32.exe") (Join-Path $out "Bin")
Copy-Item (Join-Path $root "Launcher") $out -Recurse
Copy-Item (Join-Path $root "Profiles") $out -Recurse
Copy-Item (Join-Path $root "Tools") $out -Recurse
Copy-Item (Join-Path $root "ThirdParty") $out -Recurse
Copy-Item (Join-Path $root "Release\COMMUNITY_PREALPHA_README.md") (Join-Path $out "README.md")
Copy-Item (Join-Path $root "Release\THIRD_PARTY_NOTICES.md") $out
Copy-Item (Join-Path $root "Release\release-manifest.json") $out

# Deliberately do not require or silently download wiz3D / first-person payload here.
# This package is useful for headset, injection, camera-path and executable survey tests
# without enabling unvalidated game-memory writes.
$hashLines = @()
Get-ChildItem $out -File -Recurse | Sort-Object FullName | ForEach-Object {
  $rel = $_.FullName.Substring($out.Length + 1)
  $h = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
  $hashLines += "$h  $rel"
}
$hashLines | Set-Content (Join-Path $out "SHA256SUMS.txt") -Encoding ASCII
Compress-Archive -Path (Join-Path $out '*') -DestinationPath $zip -CompressionLevel Optimal
Write-Host "Community pre-alpha package: $zip" -ForegroundColor Green