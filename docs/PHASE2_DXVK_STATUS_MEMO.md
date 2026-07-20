# WineHua Phase 2 DXVK Status Memo

> Last updated: 2026-07-21
>
> Purpose: this is the durable handoff for resuming the DXVK investigation.
> Read this file before changing DXVK, Venus present, SmokeRunner, or game launch
> code. Update it whenever a conclusion, gate result, commit, HAP, or primary
> blocker changes.

> **Current performance addendum:** the normal product D3D11 cube is visible
> and the release-candidate `shadow-precise-strong-ring` path sustains about
> 83.6 FPS. The white-window and 4.92 FPS diagnoses below are historical.
> Read
> [DXVK_GUEST_HOST_ARCHITECTURE_AND_PERF.md](DXVK_GUEST_HOST_ARCHITECTURE_AND_PERF.md)
> before changing Mesa Venus, virglrenderer shadow memory, fence feedback, or
> BrokerPresent. That document contains the current architecture, measurements,
> ranked hypotheses, and next experiments.

### 2026-07-21 frame-order and ring-publication update

The previous 30 FPS observation was the diagnostic `shadow-none` profile. It
also showed backward cube motion and is not product-correct. `shadow-precise`
removed full Host-to-Guest refresh and reached 82-90 FPS, but random Venus
command-stream decoder failures terminated the client after seconds to tens of
seconds.

The current fix adds an opt-in sequentially-consistent fence before the x86_64
Guest publishes the Venus ring tail. This is required at the Box64 boundary:
an x86 release store is normally a plain store relying on TSO, while the ring
payload can be copied by native AArch64 code. `BOX64_DYNAREC_WEAKBARRIER=0`
alone did not prevent stale or partially visible command payloads.

Validated physical-device runs:

* `frame-order-20260721-042200`: 45/45 valid, no duplicate or backward frame,
  no CS error, about 83.45 FPS.
* `frame-order-20260721-042507`: 60/60 valid, no duplicate or backward frame,
  no CS error, about 83.57 FPS, 5400+ frames, present failures=0.
* `shadow-precise-sync-submit` failed quickly and proved that asynchronous
  queue submit was not the sole cause. It remains diagnostic-only.

DXVK game and frame-order launch defaults now select
`shadow-precise-strong-ring`; WineD3D/VirGL behavior is unchanged. The 60-minute
long-run and broader real-game gate are still pending, so Phase 2 is not yet a
release completion.

Latest validated HAP: 397,782,249 bytes, SHA-256
`e2f150f49fce32d7f5f76452de05a12186d4ac3c857d6f268ffd64614e14d6e5`.

### 2026-07-20 performance update

有效 A/B 已确认：full 约 4.9-5.1 FPS；shadow-none 31.4 FPS 但存在向回转/抖动，不能产品化；Guest->Host explicit 为 5.4 FPS。explicit 模式已经把后续 64 MiB/submit 降到通常 0 B，仅保留真实的数百字节到约 90 KiB dirty range，因此剩余主瓶颈明确为 Host->Guest 128-192 MiB/fence refresh。

默认仍是 full，VirGL 回退路径未改变。下一项 P0 是 Host GPU-write range tracking 与回转 frame-order trace，而不是 DirectPresent、Modern DXVK 或进一步减少 submit。

Latest validated HAP: 397,747,812 bytes, SHA-256 638e168b256b3b3da1104fd04f7d867bbec7fb245627c1cb83f665f21c09a872.


## 0. Current conclusion

DXVK Legacy 1.10.3 is no longer blocked on D3D11 device creation, sampled-image
descriptors, or the automated D3D11 feature smoke. The x86 and x64 automated
paths create Feature Level 11.0 devices, render the deterministic scene, and
present visible output through Venus BrokerPresent.

The current blocker is narrower:

    SmokeRunner automation path: visible D3D11 frame PASS
    Normal game/product path:    DXVK renders and presents, but the visible
                                 Wine client area remains white

Do not describe Phase 2 as complete until a normal game launch produces a
physically visible D3D11 frame and the result is protected by an automated
visual gate. HRESULT success, queue-submit success, frame counters, and JSON
PASS are not sufficient by themselves.

## 1. Canonical environment and source state

    WSL distro:       Ubuntu, WSL2
    Repository:       /home/maple/Work/WineHua-build
    Windows junction: D:\MyProject\winehua-repo
    Branch:           feature/render-element-completeness
    Docker container: winehua-master-ext4
    Container source: /data/src/winehua
    Device:           5KPBB25818203996
    Bundle:           app.hackeris.winehua

Compile only inside the Docker container. Keep source and build output on WSL
ext4. Deploy with the Windows HDC from DevEco Studio.

Committed baseline:

| Component | Commit | Meaning |
| --- | --- | --- |
| Main repository | 3da7b64 | Phase 2 Vulkan and DXVK automation baseline |
| Wine | 21fac73dd92 | Current Wine Vulkan/WoW64 integration |
| Mesa | 2b1ca2f | Current guest Venus implementation |
| virglrenderer | 8e74bdf9 | Current host Venus/render-server implementation |
| DXVK fork | 49f6d9f | WineHua Legacy 1.10.3 compatibility fixes |

Before this memo commit, the main code branch was one commit ahead of its
remote. Re-check live status before every build. The DXVK fork is:

    directory: thirdparty/dxvk
    branch:    winehua/dxvk-legacy-1.10.3
    base:      v1.10.3

Do not commit these two untracked backup files:

    thirdparty/dxvk/src/d3d11/d3d11_buffer.h.orig
    thirdparty/dxvk/src/d3d11/d3d11_context.cpp.orig

## 2. Intended Phase 2 runtime

The two D3D paths remain parallel:

    WineD3D fallback
    Windows D3D -> WineD3D -> OpenGL -> virpipe -> VirGL
    -> Host EGL/GLES -> VirGL compositor

    DXVK Legacy product path
    Windows D3D11 -> managed DXVK 1.10.3 overlay -> Wine Vulkan
    -> x86_64 Vulkan Loader -> guest Mesa Venus
    -> ARM64 virglrenderer Venus -> Harmony Vulkan
    -> Venus BrokerPresent -> App compositor -> XComponent

DXVK DLLs are a WineHua-managed runtime overlay. They are not copied beside
each game and are not installed as arbitrary global system DLLs. Product
selection controls which managed overlay is injected.

Current product policy:

    fallback:      WineD3D / VirGL
    dxvk_legacy:   DXVK 1.10.3 / Venus BrokerPresent
    modern DXVK:   deferred
    DirectPresent: deferred

The Legacy profile deliberately enables relaxed feature admission for the
current Venus/Maleoon adapter. This is a compatibility profile, not proof of a
fully conformant Vulkan feature set.

## 3. Last known good automated result

Archive:

    D:\MyProject\winehua-logs\automation\phase2-20260720-133512

Result:

    appStatus=PASS
    visualStatus=PASS
    coverageStatus=PASS

HAP:

    entry/build/default/outputs/default/entry-default-signed.hap
    build time: 2026-07-20 13:33:09 +08:00
    size:       397748413 bytes
    SHA-256:    071bb0ef6ea041e67a647b103f3682b3d339c25422940a150b4c6d4f305d0e73

Important evidence:

    phase2-20260720-133512-01-dxvk-reuse/dxvk-legacy-x86.jpeg
    phase2-20260720-133512-01-dxvk-reuse/dxvk-legacy-x64.jpeg
    phase2-20260720-133512-01-dxvk-reuse/suite-summary.json
    phase2-20260720-133512-01-dxvk-reuse/hilog.txt

Both official D3D11 smoke binaries reported:

    DXVK version:                    1.10.3
    D3D feature level:               11.0
    fallbackDetected:                false
    present frames:                  60
    CPU full-frame readback/upload:  0
    per-frame vkDeviceWaitIdle:      0

The automated screenshots contain the expected color classes and geometry.
This proves that the underlying D3D11 -> DXVK -> Wine Vulkan -> Venus ->
BrokerPresent stack can render a visible frame in the SmokeRunner lifecycle.
It does not prove that the normal game lifecycle routes the same presented
image to the visible XComponent.

## 4. D3D11 functionality already covered

The official x86 and x64 D3D11 smoke currently covers and passes:

- Shader Model 5 vertex, pixel, and compute paths.
- Indexed and instanced geometry.
- Depth/stencil, blending, and rasterizer state.
- Constant buffers.
- Texture2D.Load.
- Pixel and compute shader point/linear sampling.
- Descriptor identity, rebind, unbound state, and lifetime.
- Texture arrays, mip levels, explicit LOD, and barriers.
- BC1 compatibility emulation.
- MSAA resolve.
- Queries.
- Compute, UAV, and sampled-image interactions.

Do not replace this test with a cube. The cube is a visible integration sample;
the official D3D11 smoke remains the feature-coverage authority.

Still incomplete or not yet qualified:

- Cube and cube-array subresource/view coverage.
- Format reinterpretation and a broader compatible-view matrix.
- BC2 through BC7 behavior and compatibility policy.
- Transform feedback/stream-output completeness on the relaxed profile.
- Real game compatibility breadth.
- Clean/reuse release gates and 60-minute stability gate.

## 5. Sampled-image investigation conclusion

The previous sampled-image zero-result investigation is considered resolved
for the current Legacy smoke baseline. Do not restart it from scratch unless a
new regression reproduces the old signature.

The isolation matrix established:

    Guest hand-written Vulkan sample: PASS
    Wine Vulkan hand-written sample:  PASS
    DXVK exact SPIR-V replay:          used to isolate the contract
    DXVK descriptor identity trace:   used to reconcile shader bindings
    Official DXVK D3D11 smoke:         PASS after the fork fixes

The DXVK fork now contains the compatibility work needed for this adapter,
including:

- Combined image-sampler generation for the affected DXBC sampled path.
- Matching DXVK runtime descriptor type and binding behavior.
- Texture2D.Load handling from a combined descriptor.
- Descriptor and resource identity tracing.
- Dynamic mapped-buffer synchronization fixes.
- Query and command-reset compatibility fixes.
- BC1 emulation.
- Bool sampled-descriptor specialization selected automatically for the known
  Venus/Maleoon adapter.

The old bool specialization environment variable is only a debug override.
The product behavior is now adapter/capability policy, with an explicit quirk
name available for newly encountered Venus adapters.

The relaxed profile can admit D3D11 device creation when native BC formats or
transform feedback are missing. It does not imply native implementation of all
those features. WINEHUA_FORK.md in the DXVK submodule is the policy source.

## 6. Current uncommitted work

At the time of this memo, the following main-repository files are modified:

    Makefile
    automation/Invoke-WineHuaAutomation.ps1
    automation/Start-WineHuaGameTest.ps1
    entry/src/main/cpp/wine_env.cpp
    entry/src/main/cpp/wine_exe.cpp
    entry/src/main/cpp/wine_launch.cpp
    entry/src/main/ets/pages/Index.ets
    entry/src/main/ets/service/SmokeRunner.ets
    scripts/assemble.sh

New untracked source:

    smoke/winehua_d3d_switch_cube.c

What these changes do:

1. Product/game DXVK launches receive the same guest Vulkan Loader, Venus ICD,
   Box64 library path, emulated Vulkan library list, and Venus runtime settings
   as SmokeRunner.
2. Product defaults are applied before caller-provided environment values, so
   a smoke run can retain its own run-specific log path.
3. DXVK desktop/game launch requests Vulkan present mode.
4. DXVK game launch selects Venus BrokerPresent.
5. A game working directory is derived from the executable directory. This is
   required for applications such as ComputeMark that load local shaders or
   dependencies.
6. Windows paths sent through HDC Want are converted to forward slashes because
   HDC consumes backslashes.
7. The x86/x64 D3D9/D3D11 switchable cube is packaged under C:\smoke and can be
   run in automation mode.
8. Automation coverage ignores the visual cube for exhaustive feature metrics;
   at least one official D3D11 smoke with the full metric schema is still
   required.

The cube source also suppresses unnecessary device/swapchain recreation caused
by initial WM_SIZE messages. The final initial-WM_SIZE reset exists in source,
but was only tested by a temporary device push after the last full HAP build.
It must be rebuilt into a new HAP before the next authoritative result.

## 7. Current blocker: normal DXVK window is white

Manual observations:

    D3D9 cube:  starts and is visible through the VirGL path
    D3D11 cube: D3D11 device and swapchain are created, but client area is white
    ComputeMark: did not start successfully in the earlier product path test

For the normal D3D11 cube launch, logs already show:

    D3D11CreateDeviceAndSwapChain = S_OK
    Feature Level = 11_0
    DXVK D3D11 and DXGI logs exist
    Venus queue submit returns success
    Venus present returns success
    Window title continues to update

Physical screenshot:

    D:\MyProject\winehua-cube-dx11-final.jpeg

This makes a generic DXVK device-creation failure, shader compiler failure, or
global Venus present failure unlikely. The leading hypothesis is a presentation
target/layer mismatch in the normal WineWindowAbility lifecycle:

    Venus presenter writes to SurfaceQueue A
    visible XComponent samples SurfaceQueue B

An alternative is that the white Wayland client/SHM layer is composed above the
Venus BrokerPresent consumer layer. These are hypotheses, not confirmed root
causes.

Known lifecycle difference:

    SmokeRunner:
      automationMode=true
      WineWindowAbility and XComponent created under smoke orchestration
      Venus BrokerPresent callbacks arrive
      device snapshot shows the deterministic rendered frame

    Normal game:
      automationMode=false
      launched through runWineProgram from game/product path
      WineWindowAbility and XComponent are created
      Venus present reports success
      device snapshot shows a white Wine client surface

## 8. Immediate investigation order

### P0. Trace surface identity end to end

Add one correlated, grep-friendly identity record at each boundary:

    Wine Wayland toplevel/window id
    Wine Vulkan private surface id
    PRESENT_RESOURCE context id and resource id
    PRESENT_RESOURCE surface id and presentation id
    VenusSurfacePresenter target NativeWindow identity
    Broker SurfaceQueue producer identity
    App NativeImage consumer identity
    WineWindowAbility XComponent surface id
    renderer attach/detach generation

Use a shared launch/run correlation id and surface generation. Handle-is-not-null
logs are insufficient. The trace must prove that the image rendered by DXVK is
the image sampled by the currently visible XComponent.

Compare a normal-game trace against the known-good automation trace:

    D:\MyProject\winehua-logs\automation\phase2-20260720-133512\
      phase2-20260720-133512-01-dxvk-reuse\hilog.txt

Primary grep markers:

    [VENUS-PRESENT][NCP]
    [MW-NAPI] createRenderer
    onSurfaceCreated
    surfaceId=
    [MW-COMMIT]
    attach
    detach

### P1. Prove or reject layer occlusion

Inspect the normal game mode for simultaneous layers:

    white Wayland SHM/window background
    Venus BrokerPresent NativeImage consumer

Temporarily suppress or make the white Wayland backing layer transparent in a
diagnostic build. If the DXVK frame becomes visible, correct the compositor
ownership/order instead of adding a permanent special-case overlay.

### P2. Make visible output authoritative

Rebuild the current cube into the HAP and add a physical screenshot gate for
normal game/product launch. The gate must fail for an all-white client ROI even
when D3D11CreateDeviceAndSwapChain and Present return success.

Retain the official D3D11 smoke as the feature gate. The cube gate verifies only
product launch, renderer routing, and visible presentation.

### P3. Retest ComputeMark

After P0-P2 pass, run:

    C:\ComputeMark_1.1\ComputeMark.exe

It is a 32-bit D3D11/DirectCompute application. Expected local dependencies
include:

    d3d11.dll
    d3dx11_42.dll
    D3DCompiler_42.dll
    MSVCR90.dll
    fluid3D.hlsl

The launcher must use:

    workingDirectory=C:\ComputeMark_1.1

Collect:

    C:\smoke\results\desktop-dxvk\ComputeMark_d3d11.log
    C:\smoke\results\desktop-dxvk\ComputeMark_dxgi.log
    current wine_stderr log
    physical screenshot

Classify a missing application dependency separately from a DXVK/Venus error.

## 9. Phase 2 completion work after the white-screen fix

The agreed priority is to ship a stable Legacy baseline, not to delay Phase 2
for DirectPresent, zero-copy, or Modern DXVK.

Required remaining sequence:

1. Close the remaining subresource checks: cube/cube-array and compatible
   format views. Keep mip, array, explicit LOD, and barrier regressions active.
2. Start a small real-DX11-game matrix in parallel and turn newly discovered
   failures into focused smoke coverage.
3. Confirm the bool-spec adapter quirk remains automatic and does not require a
   user environment workaround.
4. Run release gates: clean prefix once, reuse prefix three consecutive times,
   overwrite-install refresh, and 60-minute stability.
5. Finish the Legacy product selector and verified fallback to WineD3D on a new
   process launch.
6. Produce the BC2-BC7 support/emulation/unsupported matrix.
7. Preserve VirGL regressions: OpenGL x86/x64, desktop/taskbar, Z:, windows,
   audio x86/x64, Mahjong, Rich4, and Pal2.

Deferred to the next phase:

    Venus DirectPresent
    true cross-stack zero-copy
    Modern DXVK qualification
    D3D12 / VKD3D

## 10. Resume checklist and commands

First read this memo and inspect changes:

    wsl -d Ubuntu -- bash -lc "cd /home/maple/Work/WineHua-build &&
      git status --short --branch &&
      git submodule status &&
      git -C thirdparty/dxvk status --short --branch"

Run the known DXVK automation suite:

    powershell -ExecutionPolicy Bypass -File
      \\wsl.localhost\Ubuntu\home\maple\Work\WineHua-build\
      automation\Invoke-WineHuaAutomation.ps1
      -Suite dxvk -Prefix reuse

Launch the packaged visible D3D11 cube through normal game mode:

    powershell -ExecutionPolicy Bypass -File
      \\wsl.localhost\Ubuntu\home\maple\Work\WineHua-build\
      automation\Start-WineHuaGameTest.ps1
      -D3DBackend dxvk_legacy
      -GamePath C:\smoke\x64\winehua_d3d_switch_cube.exe

Launch ComputeMark after the visible cube passes:

    powershell -ExecutionPolicy Bypass -File
      \\wsl.localhost\Ubuntu\home\maple\Work\WineHua-build\
      automation\Start-WineHuaGameTest.ps1
      -D3DBackend dxvk_legacy
      -GamePath C:\ComputeMark_1.1\ComputeMark.exe

Before trusting a rebuilt package:

1. Inspect Docker mounts.
2. Build only in winehua-master-ext4.
3. Verify build exit code and signed HAP timestamp.
4. Verify HAP SHA-256 and embedded wine-data payload.
5. Verify guest Vulkan/DRI binaries are x86-64 and host libraries are AArch64.
6. Install through Windows HDC and require install bundle successfully.
7. Capture a physical device screenshot and archive logs.

## 11. Commit policy

Do not commit the current implementation changes until:

- The normal D3D11 cube is visibly rendered from a newly built HAP.
- The all-white screenshot case fails the visual gate.
- Official x86/x64 DXVK smoke still passes.
- Default WineD3D/VirGL desktop behavior is not changed into a test-only
  fullscreen/game layout.
- ComputeMark has either passed or has a precisely classified independent
  dependency failure.

Then commit in reviewable units:

1. Managed DXVK product launch environment and working-directory handling.
2. D3D9/D3D11 cube source, packaging, and smoke-manifest integration.
3. Surface routing/compositor fix and normal-game visual gate.

Update this memo with the new commit hashes, HAP hash, archive path, confirmed
root cause, and remaining gates immediately after those commits.
