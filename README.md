# ChaosVR v9 community pre-alpha engineering baseline

This pack is the current offline-development baseline for a roomscale VR conversion of **Splinter Cell: Chaos Theory**.

> **Community testers:** this repository now includes a fail-closed Windows hardware-survey build path. See `Release/COMMUNITY_PREALPHA_README.md` and `.github/workflows/publish-community-prealpha.yml`. It is **not yet a finished playable VR beta**.

## Current target

```text
SplinterCell3.exe (32-bit DX9)
  -> pluggable stereo source: wiz3D geometry SBS/TAB OR depth-stereo fallback
  -> Windows Graphics Capture -> ChaosVR Host64
  -> OpenXR / VDXR -> Quest 3

Quest HMD/controllers
  -> OpenXR / Host64
  -> shared memory
  -> Bridge32
  -> Chaos Theory camera/body/weapons
```

**Osiris is not in the required chain.** Host64 owns OpenXR itself.

## Roomscale policy

- physical HMD yaw turns Sam's body
- smooth right-stick turn adds artificial yaw
- camera must not apply physical yaw twice
- controller poses remain independent from the head/body
- 25 cm horizontal roomscale leash progressively moves the body under the player
- vertical HMD travel remains local and can drive the game's native crouch input

## What is simulated now

Run `Simulation/simulate_v3.py` to check:

- body-follow-head yaw composition
- independent controller aim
- artificial turn coordinate transform
- continuous roomscale body leash
- crouch hysteresis
- two-hand rifle latch hysteresis
- release-velocity estimator
- Chaos Theory native throw-speed mapping
- SBS eye split / quad sizing
- 32/64-bit base shared-state ABI (192 bytes), controller-extras ABI (192 bytes), camera-telemetry ring ABI (2080 bytes), and exact source-camera timing ring ABI (1056 bytes)
- WGC-QPC -> game-camera source-pose correlation
- historical source-pose interpolation for later reprojection
- stereo backend failover hysteresis (geometry -> depth -> mono diagnostic)
- captured-frame age gating (fresh -> reproject -> reject/fade)
- SBS and TAB odd-dimension eye-rect planning
- all 33 documented Chaos Theory camera modes map to explicit VR policies
- camera-mode transition recenter behavior
- wildcard signature scanner behavior for version-safe Bridge32 discovery

## Folder guide

- `Core/`: engine-agnostic pose, roomscale, weapon, stereo and frame-loop logic
- `Protocol/`: Host64 <-> Bridge32 tracking ABI plus Bridge32 -> Host64 camera telemetry ring
- `Host64/`: concrete Windows/OpenXR/D3D11 compositor skeleton, build manifest, diagnostics and Phase-A test plan
- `Tools/`: non-destructive PC preflight/inspection scripts
- `Docs/`: compositor, latency/reprojection, game bridge, research findings and ordered hardware tests
- `Simulation/`: deterministic tests that run without the game/headset

## What cannot be proven offline

1. which stereo backend is best on Chaos Theory's exact DX9 renderer (geometry vs depth) and NVG/thermal paths
2. WGC + VDXR timing/format behavior on the user's Windows/NVIDIA machine
3. exact camera/weapon injection addresses/functions in the Ubisoft executable
4. animation/bone behavior with live controller poses

Those are intentionally isolated so the first real hardware session can test one variable at a time.

## Status clarification (2026-09-24)

Host64 has now advanced from pseudocode into a Windows-targeted C++20 OpenXR/D3D11/WGC application skeleton with a vcpkg manifest. The portable protocol/math/regression suite passes, but the real Chaos Theory -> Host64 -> VDXR -> Quest chain has **not** yet been Windows/hardware-validated because the user's PC/headset are unavailable during this development pass. See `Docs/CURRENT_STATUS.md` and `Host64/PHASE_A_TEST_ORDER.md`.

## Stereo renderer deep-dive update — 2026-09-25

Stereo is no longer treated as a single wiz3D-or-depth decision. The quality-first escalation is:

1. wiz3D generic DX9 geometry stereo
2. Chaos-Theory-specific matrix/shader profile (Vireio/wiz3D model)
3. custom ChaosVR D3D9 two-eye matrix bridge
4. depth stereo fallback

New artifacts:
- `Core/OffAxisStereoProjection.cs` — runtime-FOV-derived parallel/off-axis eye projection math
- `Docs/STEREO_RENDERER_DEEP_DIVE.md` — PS3 native-stereo evidence, SM1.1/SM3/HDR triage,
  shader/pass classification and first-hardware decision tree

The deterministic simulation now checks asymmetric OpenXR FOV -> D3D off-axis projection and
renderer-stage failure classification.

## Public-release work

The engineering pack now contains a shippable-layout scaffold:

- `Bridge32/`: x86 DLL runtime/heartbeat that reads Host64 tracking without using the D3D9 proxy slot
- `Injector32/`: x86 `LoadLibraryW` injector for `SplinterCell3.exe`
- `Launcher/`: reversible install + launch orchestration
- `Release/`: user README, dependency notices, manifest and Windows staging script
- `ThirdParty/`: explicit payload locations for wiz3D DX9/x86 and the redistribution-approved first-person component

Actual camera/weapon writes remain fail-closed until retail executable signatures are validated on hardware.

### New isolation/diagnostic path

`ChaosVRHost64 --test-pattern` now exercises the actual D3D11/OpenXR swapchain
without launching Chaos Theory. The left eye receives a red checker pattern and
the right eye a green checker pattern. This is now hardware test zero: if it fails,
there is no reason to debug wiz3D/WGC/Bridge32 yet.

Bridge32 also performs a read-only PE/string-anchor discovery pass after injection
and reports its heartbeat through `Local\ChaosVR_BridgeStatus_v1`. Camera/weapon
writes remain disabled until validated signatures are found.


## 2026-09-25 timing/stereo hardening

- Bridge32 can be injected before the game entry point and intercept successful D3D9 Present calls without owning `d3d9.dll`.
- Host64 can correlate WGC compositor timestamps to exact successful Present IDs before selecting camera/tracking telemetry.
- Half-SBS/TAB and full-SBS/TAB packing are now distinguished so compressed stereo output is displayed at the correct aspect.
- A 1.2-million-frame deterministic viability stress model is included in `Simulation/simulate_viability.py`.


## 2026-09-25 camera ownership + 6DoF weapon pass

Bridge32 now contains a portable game-side VR solve rather than only tracking transport:

- explicit OpenXR -> canonical transport conversion before shared memory
- Sam-world-yaw-aware room recenter + body-follow camera ownership
- mode-aware ownership release/reacquisition
- independent grip and aim controller paths
- rifle two-hand latch + two-bone arm IK
- native spread/recoil rebasing and native-distance controller target rebasing
- wall-safe tracked muzzle fire-origin clamp
- fail-closed per-feature executable/binding capability gate
- read-only relative xref discovery breadcrumbs for the live retail executable

See `Docs/CAMERA_WEAPON_OFFLINE_PASS_2026-09-25.md`. Camera/weapon **memory writes remain
disabled** until the user's exact retail bindings are validated.


## 2026-09-25 v7 live-integration preparation

The remaining offline uncertainty has been reduced further:

- runtime SHA-256 executable-profile selection now exists in Bridge32; identity, PE metadata, unique pattern matches and semantic validation are all required before per-feature memory-write capability is granted
- first-run read-only discovery now emits relocation-tolerant candidate signatures and a profile-template generator can turn the discovery JSON into an identity-only, fail-closed profile without transcription
- D3D9 render diagnostics distinguish fixed-function VIEW/PROJECTION from shader camera-like matrices and report the actual hook providers (including proxy ownership)
- an **explicit developer-only fixed-function HMD camera experiment** can compose physical headset rotation onto `D3DTS_VIEW`; positional translation is a second opt-in switch and defaults to 100 game units/m until live scale is verified
- `fixecam`/`freecam` debug binds can be installed reversibly to isolate native camera motion from the HMD render override
- the complete offline regression is now one command: `python Simulation/run_all_offline.py`

Latest optimized snapshot: **41 core Python checks + 4 viability assertions + profile-template safety test + 23/23 portable C++ tests PASS**. A new 100,000-case native-camera/HMD composition Monte Carlo is included.

The experimental renderer override is OFF by default and does not bypass the ordinary game-memory write gate. Windows-specific Bridge32/Host64 code still requires MSVC/Windows SDK compilation on the live PC; this Linux environment cannot validate those Win32/D3D/OpenXR translation units.

## 2026-09-25 v8 offline-max update

The offline engineering path now also includes:

- a guarded shader-constant camera override fallback for one explicitly selected rigid-VIEW register
- Quest A/B/X/Y, thumbstick-click and menu button transport in the existing tracking ABI
- versioned tracking capture/replay (`.crp`) containing canonical HMD/controller state and controller velocities
- replay health analysis for cadence, integrity, tracking/focus loss, pose jumps, button edges and controller velocity
- an optional non-blocking D3D9Ex -> D3D11 direct-frame sharing protocol; Bridge32 reports live D3D9Ex eligibility and WGC remains the fallback
- automated first-run recommendation tooling that selects fixed-function, shader-camera or EPlayerCam discovery branches conservatively
- diagnostic producer/parser contract tests so logging changes cannot silently break first-run analysis

Latest optimized offline snapshot: **41 core checks + 4 viability assertions + profile safety + diagnostic/replay/recommendation contract tests + 27/27 portable C++ tests PASS**.

## v9 community pre-alpha path

A fail-closed community hardware-survey build path is now included. `.github/workflows/community-prealpha.yml` runs the complete offline regression and then builds Host64 (x64), Bridge32 (x86), and Injector32 (x86) on a Windows runner. `Release/BUILD_COMMUNITY_PREALPHA.ps1` stages a tester ZIP without pretending optional wiz3D/first-person payloads are present.

The WGC->Present correlation path now includes a 2.5 ms boundary confidence guard: ambiguous captures are still displayed but do not claim an exact source tracking pose for HistoricalProjection.
