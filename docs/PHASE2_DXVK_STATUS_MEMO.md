# WineHua Phase 2 DXVK Status Memo

> Last updated: 2026-07-25
>
> Purpose: this is the durable handoff for resuming the DXVK investigation.
> Read this file before changing DXVK, Venus present, SmokeRunner, or game launch
> code. Update it whenever a conclusion, gate result, commit, HAP, or primary
> blocker changes.

### 2026-07-26 Managed DXVK runtime contract and checkpoint

The product default for every Wine launch entry is now `dxvk_legacy` (DXVK
1.10.3). This applies to the desktop `LaunchClient` path, Explorer/file-manager
launches, `runWineProgram`, and the legacy `runWineExe` N-API entry. An explicit
`d3dBackend: "wined3d"` remains the compatibility fallback.

DXVK is a managed runtime overlay and is deliberately not copied into
`C:\\windows\\system32` or `C:\\windows\\syswow64`:

    files/wine/dxvk/legacy/x64/{d3d11.dll,dxgi.dll}
    files/wine/dxvk/legacy/x86/{d3d11.dll,dxgi.dll}

Each Wine child receives the same inherited environment:

    WINEDLLPATH=<managed x64>:<managed x86>:<Wine builtin paths>
    WINEDLLDIR0=<managed x64>
    WINEDLLDIR1=<managed x86>
    WINEDLLOVERRIDES=d3d11=n;dxgi=n

This means a game started from Explorer or the App file manager resolves the
managed native D3D11/DXGI pair when it creates a D3D11 device, and descendants
inherit the selection. A Wine program started outside the WineHua App (for
example through `hdc shell`) is not a supported launch boundary because it has
no Wine prefix, broker, Wayland, or managed environment.

Every Wine child now logs the selected backend, DXVK version, override, search
path, and presence of both x64/x86 DLL pairs. The 2026-07-26 physical-device
regression confirmed:

    x64 DXVK smoke: PASS, feature level 11.0
    D3D11 cube: PASS, 555 frames, angle regressions 0, about 80 FPS
    loaded modules: .../wine/dxvk/legacy/x64/d3d11.dll and dxgi.dll
    CPU full-frame readback/upload: 0/0

The complete `dxvk` suite is still reported FAIL only because the existing x86
test exceeds its 180-second timeout; the x64 path and runtime relocation did
not regress. Checkpoint commit:

    3f6bf02 checkpoint: default DXVK runtime and diagnostics

Performance work remains measurement-first. The current VirGL/SurfaceQueue
trace shows one GPU copy, approximately 14 us fence wait, 0.60 ms acquire,
0.88 ms submit, and 0.94 ms queue/present work at roughly 82 FPS. The next
experiment must split DXVK/Heaven frame time into fence, IPC, Host Vulkan,
shadow scan/memcpy, upload, submit, and present before changing synchronization.
Only after that split should we A/B device-side fence wait, dirty-allocation
tracking, and safe submit batching. Do not disable barriers, merge unknown
shadow gaps, or loosen fence lifetime for an unmeasured FPS gain.

### 2026-07-23 Heaven pipeline-create replay isolation (current)

The captured Heaven material replay still fails at `vkCreateGraphicsPipelines`
with `VK_ERROR_INITIALIZATION_FAILED` before any queue submit. The following
layout matrix was run on the physical Maleoon 910 Venus device:

    no-set, empty, small, ubo, dynamic, sampler, sampled, full: all FAIL

The small layout was corrected to fragment-only UBO stage flags. A built-in
constant fragment shader with specialization omitted also fails. Changing the
replay target from 64x64 to 64x4 (the passing depth-cube graphics replay extent)
does not change the failure. This rules out descriptor binding count/type,
unknown SpecId 1216, and target extent as the primary cause.

The Heaven replay init path is now aligned with the passing graphics replay:
minimal instance create, graphics+compute queue selection, and supported
storage-image read/write feature enablement. A temporary host trace records
every graphics pipeline's stage count, shader stage/module handles, pipeline
layout, render pass, fixed-state pointers, and the host return code under
`WINEHUA_VKR_TRACE_PIPELINE=1`. The NCP diagnostic environment currently
injects this flag for the next physical-device run; remove it after collecting
the comparison trace so normal game runs do not carry pipeline log overhead.

Latest diagnostic HAP:

    built 2026-07-23 23:07:47 +0800
    SHA-256 fe2341f0b069b00af9f9edcee3c15970d95bf87d4192b053d9cb91221cca6c5f

The device was disconnected after this build (`hdc list targets` empty), so the
aligned-init and host-trace A/B is not yet validated. On reconnect, install
this HAP and run `venus-heaven-material-layout` with the captured final
vertex/fragment SPIR-V. Archive `hilog.txt` and compare the Heaven records with
`venus-depth-cube-graphics` golden records before changing DXVK or Vulkan
semantics.

### 2026-07-23 Heaven Host constant-buffer identity result (current)

The Host-side Venus buffer identity trace is now implemented and committed:

    virglrenderer: e37023d3 venus: trace host constant buffer identity
    main:          7e2c254  submodule: add Venus constant buffer tracing

It records the Guest `VkBuffer`, Host `VkBuffer`, bound Guest/Host memory,
descriptor offset/range, absolute allocation offset, and matching FNV-1a hashes
from the OHOS shadow and Host mapped memory. Hashing is diagnostic-only under
`WINEHUA_VKR_TRACE_SAMPLED=1`; the normal product profile does not pay this
cost.

The ARM64 native/HAP build passed. The validated artifact is:

    HAP SHA-256:
      2ccfd619576dcb599e1f7db42236c43fe471c4d13b7e3e99cce56c2544ca3eba
    embedded wine-data.zip:
      matches 35beaa3e8b297949cd126d6adda97fa576dffdcee37b2de1b7d03186d8c8cd7a
    Guest EGL: x86-64
    Host libentry.so: AArch64

The ordinary `shadow-precise-strong-ring` DXVK reuse suite remains PASS:

    D:\MyProject\winehua-logs\automation\phase2-20260723-184718

The real Heaven `shadow-trace` run reproduced the black/missing scene while
the Host buffer trace produced:

    D:\MyProject\winehua-logs\manual\heaven-cbtrace-20260723-1853

    buffer descriptor records: 8429
    distinct Guest buffers:    741
    uniform buffer (type 6):   7221
    dynamic uniform (type 8):  1208
    hashEqual:                 8429
    hashMismatch:              0
    unavailable/invalid/empty: 0

This substantially rules out Guest shadow to Host mapped-memory corruption,
wrong buffer-memory binding, and descriptor offset overflow as the Heaven
root cause at descriptor update time. Together with the earlier descriptor,
dynamic-offset, image-identity, barrier, and conservative full-sync results,
do not continue changing shadow synchronization for correctness.

The remaining P0 is the ordinary pass-2 material shader, especially
`FS_56289d3e1ccd04a77c3d954c5ea8fe76a545a831`. Its pre-remap dump validates,
uses six ordinary implicit-LOD samples, and requests float-control execution
modes. Capture the final binary after binding remap and bool-specialization
freezing, then replay or reduce that exact binary on Venus and a conformant
reference implementation. If the final binary succeeds in isolated Venus,
resume draw-time resource-state/command-order correlation; if it fails only on
Maleoon/Venus, add a narrow capability/driver quirk and a dedicated regression
smoke rather than relaxing Vulkan behavior globally.

The current game launcher still serializes each environment override as two
Want parameters. On this device, a larger trace request can be silently
truncated and HDC can return success without starting the Ability. Pack the
environment map as one URI-encoded JSON parameter, as already done for game
argv, before relying on arbitrary trace overrides for automated exact replay.
This is an automation defect, not evidence for the rendering failure.

### 2026-07-23 Heaven mini-pipeline and Sarek strategy update (current)

The D3D11 smoke now includes a combined Heaven-style mini pipeline rather than
only isolated feature probes:

    3 x MRT + writable D24
      -> G-buffer and depth SRV
      -> RGBA16F lighting
      -> half-resolution bloom/downsample
      -> tone-map to R8G8B8A8

The physical ARM64 device passed this path in both x86 and x64 Wine:

    Session: phase2-20260723-124318
    Archive: D:\MyProject\winehua-logs\automation\phase2-20260723-124318
    Wine commit: 5903b075e31
    Main gitlink commit: 8c5cfb7
    HAP SHA-256:
      129a74728bd44f0a2eb255c1acd733282edae3cafc41fa1d7768bbe9be5c3644

    x86 duration: 10888 ms
    x64 duration:  9689 ms
    mini values:
      0xff0c0c84, 0xff17a717, 0xffbb2121, 0xffc7c7c7
    mini mismatches: 0
    fallbackDetected: false
    visible cube: 545 frames, angleRegressions=0

This rules out the basic combination of MRT ordering, D24 render/SRV use,
RGBA16F lighting, bloom downsample, and tone mapping as a sufficient
explanation for Heaven's corruption. Do not keep expanding generic smoke before
capturing evidence from the real workload. The next diagnostic is a gated dump
of one selected Heaven frame, with per-pass attachment metadata and selected
intermediate images. Locate the first incorrect G-buffer, depth/shadow, HDR,
bloom, pre-tone-map, or final image and fix only that contract.

Proton-Sarek and DXVK-Sarek were reviewed as compatibility references. Adopt
their narrow GPU/driver quirk principle, but do not copy their broad feature
optionalization into WineHua Stable. In particular:

* The Mali unbound-texture optimization change is useful only as an
  environment-controlled Heaven A/B until real-frame evidence proves it fixes
  the scene.
* Missing capabilities must remain classified as native, semantically
  emulated, or unsupported. Merely allowing D3D11 device creation does not
  implement the missing feature.
* Do not use `MESA_VK_VERSION_OVERRIDE=1.4`, ignored/relaxed barriers, or broad
  optional feature declarations as product defaults.
* A future Legacy Compatibility/Aggressive mode may prioritize startup and
  record visual-risk warnings, but it remains separate from Stable correctness.

The existing WineHua bool-specialization, BC expansion, custom-border, and
Maleoon CubeArray Dref paths follow the desired narrow, test-backed model and
remain unchanged.

### 2026-07-23 BC4/BC5 matrix and physical-device automation update (current)

The BC compatibility smoke now covers BC1, BC3, BC4, and BC5 in UNORM/SRGB or
SNORM variants as applicable, with mip0 and mip1 and signed negative/zero/positive
sample values. The SNORM vector was corrected so all sampled texels use the
intended compressed index; the earlier apparent BC4/BC5 SNORM failure was a test
vector bug, not a DXVK decoder failure.

The latest physical ARM64 device result is:

    Device: 5KPBB25818203996
    HAP SHA-256: b923bf15c774da2262ffec74714ed9695859d891f052e0db92e7cf3247d0a504
    wine-data.zip SHA-256: b59bf01e9b34e3d6f6612a1f795ff0476ac7b7b744a9d2215fd5d7aa0d5ca109
    HAP embedded wine-data.zip: matches assembled zip

Archives:

    D:\MyProject\winehua-logs\automation\phase2-20260723-044615
      explicit physical-device DXVK reuse run: PASS
    D:\MyProject\winehua-logs\automation\phase2-20260723-044951
      automatic physical-device selection DXVK reuse run: PASS

Both x86 and x64 passed the full D3D11 coverage and fixed-frame visual gates.
The result includes descriptor identity, mip/array/cube/cube-array, barriers,
MSAA, compute/UAV, depth/stencil, BC coverage, and present checks. Both
architectures report `cpuReadBytes=0`, `cpuUploadBytes=0`, and the visible cube
reports `angleRegressions=0`. The matrix is machine-readable through
`bcFormatMatrixValues`; the latest values are:

    BC1_UNORM  mip0=0xff0000ff mip1=0xff00ff00
    BC1_SRGB   mip0=0xff0000ff mip1=0xffff0000
    BC3_UNORM  mip0=0xff0000ff mip1=0xffff0000
    BC3_SRGB   mip0=0xff0000ff mip1=0xff00ff00
    BC4_UNORM  mip0=0xff0000ff mip1=0xff000080
    BC5_UNORM  mip0=0xff00ffff mip1=0xff00ff00
    BC4_SNORM  mip0=0xff000000 mip1=0xff0000ff
    BC5_SNORM  mip0=0xff000000 mip1=0xff0000ff

The ordinary `R32_FLOAT` comparison-sampler probe still returns all-true and is
retained as a diagnostic failure only. D32 and D24S8 comparison paths, including
array/cube/cube-array/view/barrier coverage, pass and are the relevant depth paths
for current Heaven investigation. Do not add a global comparison-sampler
workaround from the R32 diagnostic alone.

The automation script previously selected the first HDC target, which can be
`127.0.0.1:5555` and rejects an ARM64 HAP with an ABI mismatch. It now prefers a
non-localhost physical target and falls back to the first target only when no
physical target is available. The fix was validated by the no-`-DeviceId`
`phase2-20260723-044951` run.

Current real-workload status is unchanged: direct Heaven DX11 launch creates a
Feature Level 11.0 DXVK device and presents frames, but the scene remains
partially black/overexposed and runs at roughly 1.8-4.2 FPS. The next P3 work is
resource identity and state tracing for Heaven's depth/G-buffer SRV views,
compatible format views, subresource barriers, and post-processing inputs. The
stable cube and official smoke must remain green while doing this.

### 2026-07-23 CubeArray off-axis qualification (current)

The off-axis CubeArray qualification is now complete on the physical device.
The D3D11 smoke writes distinct D24 values into the upper-right texel of every
cube face, then uses non-center directions, ordinary SampleLevel, explicit
SampleCmpLevelZero, implicit SampleCmp, and a four-tap GatherCmp against the
same TextureCubeArray resource.

Result: x86 and x64 both PASS with mismatches=0. The 12 expected values are
0xffffffdf for the high-depth faces and 0xff000020 for the low-depth faces.
This validates face selection, U/V orientation, cube index mapping, mixed
descriptor access, comparison-gather execution, and the Maleoon 2D-array
alternate-view path. The native CubeArray Dref path remains disabled on Maleoon because it hangs the Venus
ring.

The captured FS_d10340c27c9daa60f6953469db6f3d0e956a33dc shader contains
OpImageDrefGather and a 2D, arrayed, depth image type. The same shader also
contains ordinary sample, explicit Dref, and implicit Dref instructions, so the
GatherCmp result was not optimized away.

Evidence archive: D:\MyProject\winehua-logs\automation\phase2-20260723-023746
HAP SHA-256: e4d9ca1be2bc8f6512aa56ecfc01bda2c91a251da99e99f3120b05eee2c52260
Wine commit: 5eb480fd61d

The automation summary is PASS; its release gate=false is expected because this
run was one reuse-prefix qualification, not the reuse x3, clean-prefix, and
60-minute release gate.

### 2026-07-23 CubeArray Dref root cause and Maleoon workaround (current)

The D24S8 CubeArray blocker is closed for the tested DX11
TextureCubeArray.SampleCmpLevelZero path.

The exact Guest A/B used one cube-compatible image with 12 D24S8 layers, one
comparison sampler, identical depth values and identical reference value:

    samplerCubeArray ordinary sample              PASS
    samplerCubeArrayShadow native Dref             Host Venus ring hang
    sampler2DArrayShadow with cube-to-face mapping PASS

The 2D-array Golden returned:

    0,0,0,1,1,1,1,1,1,0,0,0

It completed in 210 ms with two submits. The native CubeArray Dref is not a
descriptor, image upload, barrier, layout, array-layer, or ordinary CubeArray
filtering failure; it is a Maleoon Host Vulkan fault specific to CubeArray
shadow/Dref execution.

DXVK now enables an adapter quirk automatically when the device name contains
Maleoon:

1. The analysis pass marks only t# resources used by CubeArray comparison
   sample/gather instructions.
2. Those resources declare a 2D-array image type and request DXVK's existing
   VK_IMAGE_VIEW_TYPE_2D_ARRAY alternate view.
3. The DXBC compiler maps (direction.xyz, cubeIndex) to
   (uv, cubeIndex * 6 + face).
4. Comparison remains native OpImageSampleDref; it is not replaced by ordinary
   depth sampling followed by a scalar compare, so per-texel compare and linear
   PCF ordering are preserved.
5. Other accesses to the same shader resource use the same mapped descriptor
   type, and GetDimensions converts the six face layers back to cube count.
6. Other GPUs retain the native CubeArray path.

The archived failing fragment shader
FS_fe4234d5022c30870ab5d78b73f8638252e3dcc1 now validates as:

    OpTypeImage ... 2D ... Arrayed=1 ... Depth=1
    OpImageSampleDrefExplicitLod

Runtime tracing proves the descriptor is a 12-layer
VK_IMAGE_VIEW_TYPE_2D_ARRAY, not an accidental fallback. x86 and x64 both
return the exact 12 expected pixels with zero mismatches. The full DXVK suite,
fixed-frame visual gate, and x64 present cube also pass; the cube produced 543
frames with angleRegressions=0.

Evidence:

    Guest Golden:
      D:\MyProject\winehua-logs\automation\phase2-20260723-005718
    Full x86/x64 DXVK regression:
      D:\MyProject\winehua-logs\automation\phase2-20260723-012108
    HAP SHA-256:
      97a4f7c712ae8deca196cd9015d264caee9b00ad5f8c2256c2cdfeb2bbc885c4

Remaining qualification work is narrower than the fixed crash:

* Add off-axis face/UV patterns rather than only face-centre samples.
* Exercise implicit-LOD SampleCmp, mixed ordinary/comparison access to one
  CubeArray resource, and comparison gather.
* 2D-array hardware filtering cannot cross a cube-face seam. If a real game
  exposes a visible point-shadow seam, add a bounded seam-aware shader path;
  do not re-enable the Host instruction that hangs the ring.

### 2026-07-22 DXVK Cube Dref exact-contract investigation (current)

The native D24S8 Cube comparison probe now tests four independent contracts:

    D24S8 2DArray Dref                         PASS
    D24S8 Cube ordinary sample                PASS
    D24S8 Cube combined comparison             PASS
    D24S8 Cube separated comparison            PASS
    D24S8 Cube DXVK contract comparison        PASS

The last case uses the exact DXVK Legacy 1.10.3 image contract in a compute
shader: the descriptor variable is `OpTypeImage ... Cube 0 ...`, the sampled
image result is `OpTypeSampledImage` over `OpTypeImage ... Cube 1 ...`, and the
shader executes `OpImageSampleDrefExplicitLod`. It is not a hand-written
combined descriptor substitute. The result is `0,0,0,1,1,1`, matching the
ordinary native Dref control.

This rules out the following as the primary cause of the DXVK Cube comparison
failure:

* Venus/Host inability to execute separated Cube comparison samplers.
* The DXVK `imageTypeId` (Depth=0) to `depthTypeId` (Depth=1) SPIR-V contract.
* Generic D24S8 format creation, Cube view creation, or comparison sampler
  creation in an isolated command path.

The remaining failure is stage/runtime-specific. The DXVK fragment shader
captured in the failing run reaches `OpImageSampleDrefExplicitLod`, but its
render-pass/resource state, fragment-stage descriptor update/bind ordering,
coordinate/reference inputs, or actual D24 resource contents still differ
from the isolated probe. Continue with an exact graphics-pipeline replay and
descriptor/resource identity trace; do not globally rewrite comparison
sampling or replace it with ordinary sampling plus a scalar compare.

Evidence archives:

    D:\MyProject\winehua-logs\automation\phase2-20260722-141045
      DXVK Legacy: Cube sample PASS, Cube comparison FAIL
    D:\MyProject\winehua-logs\automation\phase2-20260722-165213
      native combined/separated/DXVK-contract Cube Dref PASS
      native CubeArray Dref still crashes in Venus/Box64 ring path

Latest diagnostic HAP SHA-256:

    18e66c77697d4991b6e91fa64a874b6cd756d10c2cbfecd99b8323d7720cd937

### 2026-07-22 Heaven DX11 direct-launch and trace-noise update (current)

Heaven can now be launched deterministically without its unstable legacy Qt
launcher:

```powershell
powershell -ExecutionPolicy Bypass -File `
  \\wsl.localhost\Ubuntu\home\maple\Work\WineHua-build\automation\Start-WineHuaGameTest.ps1 `
  -D3DBackend dxvk_legacy -GamePreset heaven-dx11
```

The launcher path is no longer authoritative automation. On the physical Pad,
`QtWebKitUnigine_x864.dll` can fault at `0xBBADBEEF` before the Win32 driver
posts the RUN click. Manual clicking sometimes wins that timing race, which
explains why manual launch appeared more reliable. The preset starts
`bin\heaven.exe` directly with the exact Direct3D 11 arguments captured from a
successful manual run and lets game mode derive the working directory from the
EXE path.

Latest direct-launch evidence:

    HAP SHA-256:
      206c6288ebec1753feb8d26c5e2f3d226f8b37a32a48e9d4dbaacfb65acfdb77
    manual trace archive:
      D:\MyProject\winehua-logs\manual\heaven-manual-20260722-1014
    direct launch archive:
      D:\MyProject\winehua-logs\automation\heaven-direct-20260722-1051
    log-quiet validation archive:
      D:\MyProject\winehua-logs\automation\heaven-direct-logquiet-20260722-1125

The direct path creates a Feature Level 11.0 DXVK device and a 640x360
BrokerPresent swapchain. Frames continue to present and there is no WineD3D
fallback, but rendering is not correct: the sky is visible while large parts
of scene geometry are black and other regions are overexposed/white. This is a
stable scene-rendering failure, not an all-white surface-routing failure and
not a captured stale frame.

The actual D32 comparison-resource smoke passes `1,0,1,0` on x86 and x64, the
Heaven comparison sampler reaches Host Vulkan with `compareEnable=1` and
`LESS_OR_EQUAL`, and traced Guest descriptor objects map to the expected Host
view/sampler handles. Do not reopen generic sampled-image or comparison-sampler
support as the leading theory. Continue with depth/G-buffer view identity,
D24S8/D32 aspect and compatible-format views, render-pass barriers/order, and
post-processing input identity.

Normal `shadow-precise-strong-ring` no longer treats trace value `0` as enabled
and no longer emits successful remote-flush, ring, fence, or queue summaries.
The 30-second Host log delta fell to about 10.8 KB instead of multiple MB; all
copies, cache operations, fences, ring barriers, and error logs remain active.
The rendering failure remained unchanged, so suppressing diagnostic I/O did
not hide or create the Heaven issue. Explicit sampled-descriptor trace is
bounded at 32,768 Host records.

> **Current performance addendum:** the normal product D3D11 cube is visible
> and the release-candidate `shadow-precise-strong-ring` path sustains about
> 83.6 FPS. The white-window and 4.92 FPS diagnoses below are historical.
> Read
> [DXVK_GUEST_HOST_ARCHITECTURE_AND_PERF.md](DXVK_GUEST_HOST_ARCHITECTURE_AND_PERF.md)
> before changing Mesa Venus, virglrenderer shadow memory, fence feedback, or
> BrokerPresent. That document contains the current architecture, measurements,
> ranked hypotheses, and next experiments.

### 2026-07-21 ComputeMark correctness and pacing update (current)

The normal DXVK game lifecycle and the previous white-window blocker are now
resolved. ComputeMark starts through the managed Legacy overlay, creates a
D3D11 Feature Level 11.0 device, renders the expected outer scene after custom
border-color emulation, and does not fall back to WineD3D. The stale white
window investigation in sections 7 and 8 is retained only as historical
context and must not be resumed as the current blocker.

The custom-border policy is capability-driven and does not key on the GPU
name: standard Vulkan colors use the standard path, native custom-border
features use the extension, the supported `SampleLevel`/LOD-0 subset uses the
shader emulation path, and unsupported combinations remain explicit. The x86
and x64 D3D11 smoke passes point and linear custom borders with zero boundary
mismatches. All 35 captured original/remapped SPIR-V binaries pass
`spirv-val --target-env vulkan1.1`.

Current validated artifacts and archives:

    HAP SHA-256:
      7574327fb6ca38e4e46793cf347c00b175dc2fe027ae7d25b0498363310866b5
    capability archive:
      D:\MyProject\winehua-logs\automation\phase2-20260721-183330
    DXVK correctness archive:
      D:\MyProject\winehua-logs\automation\phase2-20260721-183531
    optimized smoke archive:
      D:\MyProject\winehua-logs\automation\phase2-20260721-184301
    ComputeMark stable pacing archive:
      D:\MyProject\winehua-logs\manual\computemark-pacing-20260721-194956
    async-present failure archive:
      D:\MyProject\winehua-logs\manual\computemark-async-present-20260721-203229
    async-present plus synchronous-submit archive:
      D:\MyProject\winehua-logs\manual\computemark-async-present-sync-submit-20260721-204421

The ordinary D3D11 smoke remains fast: 81.934 FPS, 549 cube frames, and zero
angle regressions in the optimized archive. ComputeMark is a much heavier
DirectCompute workload and measures about 7 FPS on the current Host path. This
is not the old baseline-shadow regression:

    stable profile: shadow-precise-strong-ring
    present count 120 -> 240: 17.202 seconds, 6.98 FPS
    presenter 120-frame summary: 7.06 FPS
    present serial: 116 -> 236, monotonic
    serial_regressions: 0
    steady Host-to-Guest shadow refresh: skipped in precise mode
    steady Guest-to-Host copies at sampled submits: normally 0 bytes

The visible stutter is uneven low-throughput delivery, not fallback or old
frame replay. Frames arrive in bursts, commonly with roughly 10-15 ms between
two frames followed by a roughly 270 ms gap. On the 90 Hz display this repeats
one image for many VSyncs and then jumps to the next animation time. There are
no timestamp regressions, SurfaceQueue fallback transitions, or non-monotonic
presentation serials in the captured run.

The present breakdown identifies where the time is spent:

    present_us_avg:       131615 us
    release_wait_avg:     128317 us
    acquire_avg:             915 us
    presenter submit_avg:    657 us
    queue_present_avg:      1180 us

The synchronous release wait is a fence on the presenter copy submission. It
therefore waits for all earlier DXVK compute/render work on the same Host queue
as well as the final copy. It exposes GPU/WSI queue completion time; it is not
equivalent to a 128 ms CPU shadow memcpy. The presenter holds the Vulkan queue
external-synchronization mutex while this work completes, so the next Guest
queue submission also stalls.

Do not remove that mutex: Vulkan requires external synchronization for a
shared `VkQueue`. The existing async-present prototype was tested and rejected:

1. `shadow-precise-strong-ring-async-present` advanced only five frames, then
   hit `aborting on ring fatal error at iter 4096` and Box64 signal 6.
2. Adding `VN_PERF=no_fence_feedback,no_query_feedback,no_async_queue_submit`
   removed the ring fatal and reached 120 frames, but only improved 7.06 to
   7.46 FPS. The wait moved to `vkQueuePresentKHR` (`queue_present_avg=105484
   us`), the swapchain recreated repeatedly, and the application later left an
   empty desktop. This is a diagnostic FAIL, not a candidate default.

The next performance work must preserve the stable profile and proceed in this
order:

1. Add split timing around Host queue mutex acquisition and the actual driver
   `vkQueueSubmit`, while retaining the existing presenter stage metrics.
2. Measure Host GPU execution separately from presentation copy/WSI. Prefer
   timestamp queries or an equivalent bounded probe; do not infer GPU time
   only from a CPU fence wait.
3. Evaluate a managed `dxgi.maxFrameLatency=1` A/B to smooth burst pacing. The
   config must be supplied by WineHua runtime/manifest packaging, not copied
   beside a game. It is a pacing experiment, not an expected throughput gain.
4. Only pursue a present worker or second-queue design if explicit ownership,
   semaphore ordering, object lifetime, resize, and device-loss behavior are
   defined. A raw async callback or unlocked `VkQueue` is not acceptable.
5. Keep the 83 FPS smoke, x86/x64 correctness suite, frame-order trace, clean
   and reuse-prefix gates active while changing the present path.

Input automation is available and works. The packaged
`winehua_win32_driver.exe` locates the Win32 title/button and records
`BM_CLICK sent`; use this instead of asking for a manual click. The 12-second
delay used once on 2026-07-21 was only an attempted capture alignment. Normal
automation should start the driver after about one second and let it wait for
the window. HDC touch injection did not reliably cross the current
XComponent/Wayland input boundary.

Two automation defects discovered during this run remain open:

- Windows HDC can print `[Fail]`/`permission denied` while returning exit code
  zero; deployment helpers must validate output text as well as `$LASTEXITCODE`.
- A transient device disconnect during the force-stop polling loop is reported
  as "WineHua process did not stop". Detect and classify the HDC transport
  error, reconnect once, then retry the bounded status check.

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
* Final ordinary default launch remained live past frame 21,989 at 83.7 FPS
  with `regress 0`; the Host log reached present serial 22,069 with no CS
  error. The cube was intentionally left running on the Pad for inspection.
* `shadow-precise-sync-submit` failed quickly and proved that asynchronous
  queue submit was not the sole cause. It remains diagnostic-only.

DXVK game and frame-order launch defaults now select
`shadow-precise-strong-ring`; WineD3D/VirGL behavior is unchanged. The 60-minute
long-run and broader real-game gate are still pending, so Phase 2 is not yet a
release completion.

Latest validated HAP: 397,782,249 bytes, SHA-256
`e2f150f49fce32d7f5f76452de05a12186d4ac3c857d6f268ffd64614e14d6e5`.

### 2026-07-21 occlusion-query feedback update

The last full D3D11 smoke failure was not a zero-valued occlusion result. Both
x86 and x64 remained `VK_NOT_READY` for the full two-second timeout. Stencil
pixel readback was already correct, which separated stencil rendering from
query availability.

Venus query feedback writes result and availability words with a Host GPU copy
and then reads them directly from the Guest mapped feedback buffer. WineHua's
OHOS vtest path uses separate Guest SHM and Host Vulkan mappings. The Host write
therefore did not update the Guest availability word, which stayed zero
forever. This is the same architectural constraint that requires fence
feedback to be disabled.

The product quirk now uses:

```text
VN_PERF=no_fence_feedback,no_query_feedback
```

This keeps command-buffer query reset and routes `vkGetQueryPoolResults`
through the synchronous Host RPC. It does not remove query synchronization or
fake a result. Copying the internal feedback allocation back on every poll was
rejected because it would add Host-to-Guest memcpy, non-coherent range
alignment concerns, and new feedback-buffer lifetime coupling.

Physical-device evidence:

* A/B archive `phase2-20260721-050642`: full DXVK suite PASS.
* Formal product archive `phase2-20260721-051423`: x86/x64 PASS, no fallback,
  and both precise occlusion queries returned 42,488 samples.
* Query trace for both architectures is `reset -> begin -> end -> VK_SUCCESS`.
* Clean-prefix DXVK archive `phase2-20260721-051812`: PASS.
* Core/VirGL archives `phase2-20260721-052035` (reuse) and
  `phase2-20260721-052157` (clean): PASS.
* Frame-order archive `frame-order-20260721-051629`: 60/60 valid, zero
  duplicate, zero regression.
* Consecutive reuse gate `phase2-20260721-052627`: 3/3 suites PASS; all six
  x86/x64 queries returned 42,488 samples with no fallback.

The dedicated `dxvk-long` suite now measures wall-clock time, writes a
five-second atomic heartbeat, exposes a fixed-frame visual window, and applies
the complete D3D11 coverage gate at the end. Its 60-second infrastructure
self-test `phase2-20260721-054504` passed after 62.161 seconds and 4,484 frames.
The actual 3,600-second release run remains pending and must not be inferred
from this shorter self-test.

Latest validated HAP: 397,785,474 bytes, built 2026-07-21 05:42:59 +08:00,
SHA-256
`455e666edbaa7f0118f60cb6784041c2c96eb14aeef69c4e26441b9c3d2b3a1a`.

### 2026-07-20 performance update

有效 A/B 已确认：full 约 4.9-5.1 FPS；shadow-none 31.4 FPS 但存在向回转/抖动，不能产品化；Guest->Host explicit 为 5.4 FPS。explicit 模式已经把后续 64 MiB/submit 降到通常 0 B，仅保留真实的数百字节到约 90 KiB dirty range，因此剩余主瓶颈明确为 Host->Guest 128-192 MiB/fence refresh。

默认仍是 full，VirGL 回退路径未改变。下一项 P0 是 Host GPU-write range tracking 与回转 frame-order trace，而不是 DirectPresent、Modern DXVK 或进一步减少 submit。

Latest validated HAP: 397,747,812 bytes, SHA-256 638e168b256b3b3da1104fd04f7d867bbec7fb245627c1cb83f665f21c09a872.


## 0. Current conclusion

DXVK Legacy 1.10.3 is no longer blocked on D3D11 device creation, sampled-image
descriptors, occlusion queries, or BrokerPresent. The x86 and x64 automated
paths create Feature Level 11.0 devices and pass texture sampling, descriptor
identity/lifetime, mip and array subresources, barriers, BC1 emulation, MSAA,
compute/UAV, stencil pixel and precise occlusion-query checks.

The normal product cube is physically visible at about 83-84 FPS with no
fallback or backward frame. Remaining Phase 2 release work is the 60-minute
DXVK long run, broader real-game coverage, and the remaining BC2-BC7
compatibility matrix. D3D9-to-D3D11 hot switching in one HWND
also exposed a separate SurfaceQueue ownership handoff issue; direct D3D11 game
launch is correct, but that lifecycle case must remain a regression item.

Do not describe Phase 2 as complete until those gates pass. HRESULT success,
queue-submit success, a single cube, and JSON PASS are not sufficient by
themselves.

## 1. Canonical environment and source state

    WSL distro:       Ubuntu, WSL2
    Repository:       /home/maple/Work/WineHua-build
    Windows access:    \\wsl$\Ubuntu\home\maple\Work\WineHua-build
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
| Main repository | 6c15447 before the long-run update | Query-feedback fix |
| Wine | 4af0d72b67d | Wine Vulkan/WoW64 plus wall-clock D3D11 long smoke |
| Mesa | b2ecdc82d68 | Strong Venus ring publication barrier |
| virglrenderer | b141b55650d | Precise Host shadow synchronization |
| DXVK fork | 0cbbfa7d4c3 | WineHua Legacy 1.10.3 compatibility fixes |

Before this memo commit, the main code branch was one commit ahead of its
remote. Re-check live status before every build. The DXVK fork is:

    directory: thirdparty/dxvk
    branch:    feature/render-element-completeness
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

    D:\MyProject\winehua-logs\automation\phase2-20260721-054809

Result:

    appStatus=PASS
    visualStatus=PASS
    coverageStatus=PASS

HAP:

    entry/build/default/outputs/default/entry-default-signed.hap
    build time: 2026-07-21 05:42:59 +08:00
    size:       397785474 bytes
    SHA-256:    455e666edbaa7f0118f60cb6784041c2c96eb14aeef69c4e26441b9c3d2b3a1a

Important evidence:

    phase2-20260721-054809-01-dxvk-reuse/dxvk-legacy-x86.jpeg
    phase2-20260721-054809-01-dxvk-reuse/dxvk-legacy-x64.jpeg
    phase2-20260721-054809-01-dxvk-reuse/suite-summary.json
    phase2-20260721-054809-01-dxvk-reuse/wine-stderr.log

Both official D3D11 smoke binaries reported:

    DXVK version:                    1.10.3
    D3D feature level:               11.0
    fallbackDetected:                false
    present frames:                  60
    CPU full-frame readback/upload:  0
    per-frame vkDeviceWaitIdle:      0
    precise occlusion samples:       42488

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

## 12. 2026-07-25 Heaven draw0 exact replay A/B

The latest HAP was installed on physical device `5KPBB25818203996`:

    HAP SHA-256: 52eea041108a49966ad6e3aa034e13cb85413f94a5eca85584befe73cf47690d
    embedded wine-data.zip matches assembled payload

The new `venus-heaven-draw0` suite ran four exact variants using the same
captured vertex/fragment SPIR-V, six full-mip sampled images, ten UBOs, index
and vertex buffers, and descriptor bindings 0-21:

    D:\MyProject\winehua-logs\automation\phase2-20260725-090001

All four Venus/Maleoon runs passed pipeline creation and one queue submit:

    loaded:  checksum 0xf0feb108, changedPixels 20508
    clear:   checksum 0x2e0fd6ba, changedPixels 27076
    nodepth: checksum 0x6248cf31, changedPixels 27076
    always:  checksum 0x6248cf31, changedPixels 27076

The same four captures were rebuilt with the current replay source and run on
Lavapipe. Every variant also passed:

    loaded:  checksum 0x8fd624cb, changedPixels 14888
    clear:   checksum 0x44e63bc0, changedPixels 27076
    nodepth: checksum 0xabeac79c, changedPixels 27076
    always:  checksum 0xabeac79c, changedPixels 27076

The loaded/clear/nodepth/always ordering is therefore identical on both
drivers. The raw outputs differ between Maleoon and Lavapipe, but the device
does not lose the draw, fail pipeline creation, or ignore depth compare. The
physical logs show `vkCreateGraphicsPipelines returned 0` for all four tests,
and no device-lost or replay failure. The exact replay still uses CPU readback
only as an offline diagnostic (`cpuReadBytes=1843200`); it has no per-frame
`vkDeviceWaitIdle`.

Conclusion: the captured draw does not support a global descriptor, SPIR-V,
or generic Venus sampled-image failure. The remaining Heaven defect is in the
real frame's pass/state contract or in a later post-processing attachment. The
next diagnostic must capture per-pass color/depth/HDR/bloom metadata and the
first incorrect intermediate image, while preserving the passing draw0 suite.
Do not change stable DXVK descriptor or shadow synchronization based on this
A/B alone.

## 13. 2026-07-25 Heaven f647 RGBA16F alignment finding

The exact frame 180 / pass 2 / draw 390 replay initially appeared to fail
before any draw on Maleoon:

    expected baseline checksum: 0x1f10f6f7
    Maleoon baseline checksum:  0x3ca290f3
    changed pixels:             230400 / 230400

Raw-output comparison proved that this was not floating-point precision,
channel swizzle, render-pass load/store, or general shadow corruption:

    device[4:] == captured_golden[:-4]  (1843196 / 1843196 bytes)

The leading device bytes were `ff ff ff 00`, exactly matching the final D24S8
texel immediately before the RGBA16F capture in the shared upload buffer. The
replay's RGBA16F source offset was:

    target_upload_offset = 22893908
    target_upload_offset % 4 = 0
    target_upload_offset % 8 = 4

This is Vulkan-valid four-byte alignment, but Maleoon behaves as if the
R16G16B16A16_SFLOAT copy offset must be aligned to the full eight-byte texel
and rounds the source down by four bytes. The replay now uses the stricter,
still-valid texel-size alignment for its RGBA16F source.

Important scope limit: DXVK 1.10.3's `copyImageHostData` already allocates its
staging slice with `CACHE_LINE_SIZE` alignment. Do not add a speculative DXVK
alignment quirk based on this replay artifact. First prove an actual DXVK copy
uses an offset that is four-byte aligned but not texel aligned.

Reference verification after the replay fix:

    Lavapipe aligned baseline: PASS
    checksum:                  0x1f10f6f7
    changed pixels:            0

Artifacts:

    build/diagnostics/heaven-f647-lavapipe/result-baseline-aligned.json
    build/diagnostics/heaven-f647-lavapipe/output-baseline-aligned.rgba

The rebuilt ARM64 artifact is ready but was not installed because HDC lost the
physical target immediately before deployment:

    HAP timestamp: 2026-07-25 13:35:00
    HAP SHA-256:   744a13a7a70ce0b57c573b4235f15b17ad7bf77d32fa0375328267540f7065eb
    wine-data:     c83480cee5ab4f63dcddbeb947177d5be1e6a4c58f8dad568c0d808aec1578e3
    Guest replay:  x86-64
    Host libentry: AArch64

The next connected-device sequence is:

1. Run `venus-heaven-f647` with the default GPU shadow upload and require the
   aligned no-draw baseline to pass bit-exactly.
2. If it still fails, run the new `shadow-precise-cpu-upload` A/B profile. It
   changes only Host shadow upload from `vkCmdUpdateBuffer` to mapped flush.
3. Once the baseline passes, compare loaded/EQUAL, no-depth, and ALWAYS exact
   draw outputs against Lavapipe. Only then resume shader, blend, depth, or
   descriptor investigation.
4. Preserve the existing DXVK staging alignment until an actual DXVK trace
   proves a violating offset. Do not convert this diagnostic artifact into a
   global product workaround.

## 14. 2026-07-25 Heaven final-pass dual-source resolution

The real-frame capture reduced the visible Heaven corruption to frame 120,
pass 2, draw 666. Draws 0-665 already contain the HDR scene, materials, roads,
and buildings. Draw 666 is a full-screen triangle using:

    VS_1d847df94f10700d88316f63fcf6acbea4717f78
    FS_867aa8b9e85be8db453848a261c10fac3249e09d

The draw requests D3D11 dual-source blending equivalent to:

    result.rgb = o0.rgb + destination.rgb * o1.rgb

Maleoon reports `dualSrcBlend = false`. This is a valid Vulkan implementation
because the feature is optional. Relaxing DXVK's FL11 feature gate therefore
was not sufficient: the native dual-source pipeline had undefined behavior.

Two DXVK issues were fixed:

1. The 1.10.3 SPIR-V scanner matched `Location 1` without restricting the
   variable to `StorageClass Output`. A fragment input at the same location
   could overwrite the secondary-output record. The scanner now records only
   output variables.
2. The new two-pass emulation initially used an `unordered_set` of output IDs
   and stopped as soon as it found `o1`. When iteration visited `o1` before
   `o0`, `m_o0LocOffset` stayed zero and the secondary variant was not remapped.
   The first pass therefore calculated `destination *= o0`, and the second
   pass added `o0`. It now stops only after both output offsets are known.

The constrained emulation is enabled only when the Host lacks dual-source
blend and the draw matches the side-effect-free single-target formula used by
Heaven. It performs:

    pass 1: destination = destination * o1
    pass 2: destination = destination + o0

There is no CPU frame readback or upload. Diagnostic modes can expose `o0`,
`o1`, either individual arithmetic pass, or the default two-pass result via
`DXVK_WINEHUA_DUAL_SRC_MODE`.

Physical-device evidence:

    HAP SHA-256:       361993ef598099db6eb74499121c138de3ce4cf503efd6846241d48850cee5b0
    wine-data SHA-256: b848b3dba232d7e4c1bc63f683050130363185751c6f101214b1ee772ae178a9
    x64 d3d11.dll:     59349b3b1d5f7e17dcaeb87b232511d818e3bbddc8d67e301ee0d4587ea31661
    device DLL hash:   matches packaged x64 DLL

After the loop fix, the log contains three distinct final shader variants:

    native dual-source: 1e318dab8aee558a
    secondary output:   991e93e9aa8f258a
    primary output:     b53dc1bcd0485c2a

The default two-pass physical screenshot restores the ship's wood, metal,
ropes, normal detail, and lighting:

    D:\MyProject\winehua-logs\manual\heaven-dual-src-scan-fix-twopass-20260725

The x64 exhaustive D3D11 smoke and visible cube pass after this change. The
cube rendered 552 frames with `angleRegressions = 0`. The first x86 smoke run
timed out before producing a result and is being treated as a separate
automation/runtime regression until its single allowed retry completes:

    D:\MyProject\winehua-logs\automation\phase2-20260725-215522

Next work is no longer broad sampled-image correctness. It is:

1. Finish the x86 retry and preserve x64/x86 smoke coverage.
2. Measure Heaven frame pacing with continuous screenshots and split
   render/fence/present timing; distinguish pacing stalls from old-frame
   regressions.
3. Measure the one extra full-screen draw cost. Optimize only after the timing
   split; do not weaken the correct two-pass ordering.
4. Add a focused dual-source smoke that has both fragment input and output at
   Location 1 and checks the emulated `o0 + destination * o1` result.

## 15. 2026-07-26 frame-order investigation

The `shadow-precise-strong-ring-trace` profile now carries a read-only trace
through the render server, NCP presenter, and the main NativeImage consumer.
The trace records a common serial and monotonic timestamp for:

    enter -> source-fence-ready -> target-acquired -> copy-submitted
    -> queue-present-returned -> source-release-ready -> published

The main consumer records signal delta, coalesced callbacks, consumed image
timestamp delta, duplicate timestamps, and timestamp regressions. This does
not change fence, barrier, drop-buffer, or fallback behavior.

The continuous Heaven run is archived at:

    D:\MyProject\winehua-logs\manual\heaven-stage-trace-20260726-r3

It produced 377 NCP/main frames. The observed invariants were:

    NCP serial: 1..377, serial_regress=0
    NativeImage timestamp: monotonic, timestamp_regress=0
    signal_delta: always 1
    coalesced callbacks: 0
    duplicate timestamps: 0
    source image ring: alternating two source handles in serial order
    NCP target ring: 0,1,2,0,1,2,... in serial order

The first seven present calls with serial 1 were startup publication retries
returning `-EAGAIN` before the SurfaceQueue target attached. They did not
publish a frame and must not be interpreted as replayed history.

The frame interval distribution was approximately p50=158 ms, p95=323 ms,
p99=565 ms, with six intervals above 500 ms and two above 1 s. The largest
intervals were 17.8 s and 5.2 s. NCP present itself remained ordered and
reported no throttling; the host shadow-to-host synchronization had p50 about
26 ms, p95 about 218 ms, p99 about 324 ms, and a maximum about 484 ms.

Current conclusion: there is no evidence that an old serial or old NativeImage
timestamp is submitted again. The visible back-and-forth is currently better
explained by a long producer/shadow stall, during which the compositor keeps
the last valid buffer, followed by a large scene jump when a new frame arrives.

Do not implement stale-frame dropping, remove fences, disable precise shadow
sync, or switch to CPU fallback until a trace records a real serial or
timestamp regression. Next investigation is to split the long producer gap
between guest command generation, shadow dirty-range synchronization, and
present dispatch; optimize only the measured dominant segment.

## 16. 2026-07-26 surface-selection regression investigation

The user's latest manual run appeared to show the cube jumping between old
positions. Comparing it with the frame-order fix in section 15 found no code
rollback in the ring publication or precise shadow path: the live device log
still reported monotonic cube `frame`/`angle`, `regress=0`, and monotonic Venus
present serials. The more recent manual session had multiple Explorer/Wine
children and therefore multiple live surface entries in the NCP presenter.

The remaining lifecycle hazard was in the zero-copy consumer selection:

* NCP `Query()` exposed `unordered_map` iteration order, so the first
  unattached candidate was nondeterministic after a restart.
* The main compositor did not require the candidate's Vulkan flag to match the
  current product mode, allowing a stale GL surface to be selected for a DXVK
  process (or vice versa).

Commit `3340544` fixes only this boundary. NCP candidates are sorted by the
most recent present time, serial, and surface key; the main consumer filters by
the current Vulkan/non-Vulkan mode and releases a binding if its type changes.
Fence, shadow synchronization, queue pacing, and frame dropping are unchanged.

The rebuilt and installed HAP was verified with a clean process restart and
`C:\smoke\x64\winehua_d3d_switch_cube.exe`: 80-87 FPS, increasing frame and
angle values, `regress=0`, and no surface-type change warning. Existing
uncommitted DXVK/Heaven investigation files remain separate from this fix.

## 17. 2026-07-26 Heaven regression reset and artifact rule

The previous frame-order conclusion was too broad. Monotonic NCP present
serials and NativeImage timestamps prove that the compositor does not publish
an older SurfaceQueue buffer again. They do **not** prove that the Guest
rendered camera state is monotonic. The user continues to observe camera-angle
rollback in Heaven, including the latest clean process run.

The package described by the user as smooth at approximately 12:40 was not
archived as a HAP. The nearest retained metadata is:

    D:\MyProject\winehua-logs\automation\phase2-20260726-130728\artifact.json
    HAP SHA-256:       50d4e0610e55678d4ac7b82422bfc34e279059cc236ab92ac0705cb5f8a93544
    wine-data SHA-256: 6bd3c6ca3e0ffeda9ed63d652d8e1b92fd72cfaa097d5190074b4a2a510449ca
    main commit:       ef6e1a6f87005d3d79bdc8addba01044913818ea

The metadata is not a replaceable artifact and therefore cannot be treated as
an available known-good package. The recorded 12:33
`shadow-precise-dirty-ring-no-upload-fast` Cube run proves only Cube ordering;
it is not evidence that the same HAP passed a continuous Heaven camera test.

Fresh A/B results reset the investigation:

* `shadow-precise-dirty-ring-no-upload-fast` still rolls back visibly after
  GPU upload is disabled.
* Disabling the batch `vkFlushMappedMemoryRanges` path added after 12:40 does
  not remove the rollback.
* The retained 09:32 Guest payload and the current payload contain identical
  x64 DXVK `dxgi.dll`/`d3d11.dll` and Guest Mesa `libgallium-25.0.1.so`
  binaries. A changed DXVK or Guest Mesa binary is not the current split.
* Present serial and NativeImage timestamp traces remain useful, but only to
  exclude Host compositor replay.

The P0 diagnostic is now a continuous, low-overhead camera-buffer trace. The
candidate is Heaven pass 0, descriptor binding 1, DXVK resource slot 161,
144 bytes. Each frame must correlate:

    DXVK frame + Guest buffer handle/offset + camera hash/words
      -> Venus Host buffer/memory + dirty generation + copied hash
      -> queue submit + present serial + NativeImage timestamp

Interpretation is strict:

1. If the DXVK camera words themselves move backward, investigate the game's
   constant-buffer update sequence, DXVK dynamic-buffer slice reuse, and Guest
   CPU publication ordering.
2. If DXVK camera words are monotonic but the Host hash is older, fix Venus
   shadow dirty-range lifetime, mapped-memory visibility, or submit ordering.
3. If both hashes are monotonic, correlate the rendered attachment with the
   same submit before revisiting presentation. Do not drop frames or mask the
   visual rollback.

Artifact discipline is now a release-blocking rule for every visual-ordering
milestone:

* Archive the signed HAP itself, its SHA-256, embedded `wine-data.zip`, Guest
  DLL/ICD hashes, Host renderer hashes, main/submodule commits, and binary
  dirty diffs.
* Archive the exact launch profile and environment, continuous Heaven evidence,
  Cube frame-order result, logs, and screenshots/video in the same run folder.
* Create a local milestone commit after the evidence passes. A configuration
  note or Cube-only result is not sufficient to mark Heaven fixed.
* Never overwrite the only passing HAP; retain at least the current passing
  artifact and the immediately previous comparison artifact.

## 18. 2026-07-26 Heaven camera-buffer reuse evidence

The first camera candidate was wrong: pass 0, binding 1, resource slot 161,
144 bytes is a fixed projection matrix.  The continuously changing view/camera
matrix is pass 0, vertex uniform binding 3, DXVK resource slot 163, 48 bytes.
Binding 4 / resource slot 164 is a separate dynamic 1536-byte block.

The Guest trace from frames 119 through 249 recorded a unique camera hash on
every frame and no exact old-hash replay.  The physical VkBuffer/offset backing
that binding can be reused after only two frames; approximately 30 physical
slices served 131 frames.  Monotonic Guest hashes therefore do not exonerate
the Host mapped-memory bridge: an already queued Host shader read may still see
a later CPU overwrite of the same mapped range.

The current correctness/performance A/B is:

* `shadow-precise-dirty-ring` enables a private Host GPU upload before Guest
  submissions.  Its ALL_COMMANDS -> TRANSFER -> ALL_COMMANDS dependency and
  `vkCmdUpdateBuffer` snapshot removed the observed replay in the captured run,
  but Heaven fell to approximately 11-12 FPS because it adds a Host queue
  submission.
* `shadow-precise-dirty-ring-no-upload-fast` retains approximately 20+ FPS but
  the user continues to observe camera rollback.  Monotonic SurfaceQueue
  serials and NativeImage timestamps only exclude compositor buffer replay.
* `DXVK_WINEHUA_FIFO_BUFFER_SLICES=1` changes host-visible uniform slice reuse
  from newest-free-first to oldest-free-first.  It is a diagnostic A/B, not a
  product fix.  The user still observes rollback, so it must not be marked as
  resolved.

The working root-cause model is a mapped-shadow generation hazard rather than
a present-order problem: CPU memcpy/flush updates Host mapped memory outside
the Vulkan queue, while a short-lived DXVK dynamic slice may still be consumed
by an older Host submission.  The separate GPU-upload path snapshots the bytes
into queue order, which explains its stronger correctness evidence and its
submit overhead.

The next product candidate must preserve that queue-ordered snapshot without
one extra `vkQueueSubmit` call per Guest submit.  Preferred design: prepend a
private upload command buffer and append an internal timeline-semaphore retire
signal to the same Host `vkQueueSubmit` call as the Guest work.  The timeline
value owns upload command-pool reuse; the Guest fence remains untouched.  Do
not remove Guest fences, drop frames, or rely on FIFO slice order as the fix.

Checkpoint and evidence:

    DXVK diagnostic commit: 2c94fd5
    Run root: D:\MyProject\winehua-logs\manual\heaven-camera-20260726-2030
    GPU upload frames: frames-gpu-upload
    Fast mapped frames: frames-fast-no-upload-212817
    FIFO frames: frames-fast-fifo-214441

Every future passing milestone must archive the exact HAP, embedded/runtime DLL
hashes, profile/environment, logs, Heaven evidence, and Cube angle result before
any newer package is deployed.


## 19. 2026-07-26 inline-upload candidate rejected

The user repeatedly confirmed visible Heaven camera-angle rollback with the
installed inline-upload candidate. It is now a failed baseline, not a passing
milestone:

    profile: shadow-precise-dirty-ring-inline-upload
    HAP SHA-256:
      9aa4428b403433975f50db72d47631686e03c6133b9ed5acdc8f070bc608dece
    archive:
      D:\MyProject\winehua-logs\manual\heaven-inline-upload-20260726-221238

The archived `artifact.json` is marked
`FAIL-user-observed-camera-rollback`. Screenshot sampling that did not catch a
short rollback is not evidence of correctness. Guest camera hashes being
unique is also insufficient: uniqueness excludes exact byte replay but does
not prove that decoded camera motion is monotonic or that one rendered frame
uses one coherent constant-buffer generation.

A code audit found that queue-ordered `vkCmdUpdateBuffer` does not yet remove
the original mapped-memory hazard. `vkr_device_memory_flush_shadow_range`
currently copies every Guest flush directly into `mem->host_map` immediately.
The later queue-submit path records a snapshot upload and has both pre- and
post-transfer barriers, but the earlier CPU `memcpy` can already overwrite
Host memory still consumed by an older GPU submission. The latest cumulative
profile confirms that this path remains active:

    shadow bytes copied through mapped Host memory: about 40.3 GB
    queue-ordered upload bytes:               about 13.8 GB
    safely skipped Host-copy bytes:           about 1.63 GB

The working root cause is therefore refined to **immediate Host mapped-memory
overwrite before queue ordering**, not missing SurfaceQueue ordering and not a
missing post-upload Vulkan barrier.

The next correctness A/B must:

1. Snapshot Guest flush contents outside the live Host VkDeviceMemory.
2. Defer writes to the real Host buffer until the private upload command is
   ordered between prior GPU work and the matching Guest submissions.
3. Prove every dirty byte is covered by transfer-destination buffers. Any
   uncovered range must use an explicitly synchronized correctness fallback;
   it must not silently restore the immediate unsafe memcpy.
4. Decode the camera data or continuously analyze scene motion. Hash
   uniqueness, present serials, timestamps, and sparse screenshots are only
   supporting evidence.
5. Archive the signed HAP, hashes, exact source state, profile, logs, continuous
   Heaven evidence, and Cube angle result before calling the candidate fixed.

The unbuilt bound-buffer dirty-list optimization is not eligible for deployment
yet. Audit found two lifecycle bugs in the working tree: `bound_buffers` is not
initialized at memory creation, and dirty-list removal incorrectly reinitializes
that independent list. It must be fixed and separately gated so the first
deferred-copy correctness A/B is not confounded by a performance-path change.


## 20. 2026-07-26 deferred Host-copy correctness candidate

A new candidate was built, archived before installation, and overwrite-installed:

    profile: shadow-precise-dirty-ring-inline-upload
    HAP SHA-256:
      fa5b6d848a90a9c467b36112f747a0220777bef00afd4a0d14ede4027ddb04c7
    wine-data SHA-256:
      541e9fb6c3492d1cd1210d2edb00573e09b57a34700235817892efccbf2b68ef
    archive:
      D:\MyProject\winehua-logs\manual\heaven-deferred-shadow-20260726-225828

This candidate snapshots Guest flush contents separately and does not copy them
immediately into live Host VkDeviceMemory. Fully covered buffer ranges are
published only through the queue-ordered upload command. An uncovered deferred
range uses an internal same-queue idle wait before the CPU fallback.

The physical-device run confirmed:

    WineHua shadow GPU upload inline-submit enabled
    shadow_copies=0
    shadow_bytes=0
    deferred Host-copy queue-wait fallback count=0
    Heaven materials, geometry, and lighting complete
    HUD approximately 20 FPS

Fifty display frames captured at a 303 ms median interval covered about 15
seconds and two Heaven scenes. The similarity-order check found zero frames
that were materially closer to an older history frame than to the immediately
preceding frame. Frames 30-32 are the normal scene fade. This is supporting
evidence only: the artifact remains
`TESTING-AUTO-50-PASS-USER-PENDING` until the user confirms that short
full-rate camera rollback is absent.

The current Heaven child diagnostic string still says
`WINEHUA_PERF_PROFILE=shadow-precise-strong-ring` because
`AppendStableDesktopDxvkEnv` overwrites only the label after the selected host
profile has already reached the NCP. The live renderer nevertheless proves the
inline path is active. Source has been adjusted to preserve the selected label;
that diagnostic-only change is not present in the archived candidate HAP.

Performance work remains separate. The bound-buffer fast iteration is still
disabled, and the current dominant measured cost is upload command preparation
and object-table buffer scanning. Do not enable it until Heaven correctness and
Cube ordering are both accepted.


## 21. 2026-07-26 deferred Host-copy candidate rejected

The user confirmed that Heaven still continuously shows camera-angle rollback
with the installed deferred-copy candidate. It is therefore rejected:

    profile: shadow-precise-dirty-ring-inline-upload
    HAP SHA-256:
      fa5b6d848a90a9c467b36112f747a0220777bef00afd4a0d14ede4027ddb04c7
    archive:
      D:\MyProject\winehua-logs\manual\heaven-deferred-shadow-20260726-225828
    status:
      FAIL-user-observed-camera-rollback

The 50-frame screenshot result in section 20 was a low-rate supporting check,
not a correctness gate. It sampled the display at about 3.3 FPS and could miss
a short rollback between samples. It must never be used to overrule continuous
full-rate observation.

The deferred Host-copy experiment still proves one useful negative result: the
rollback survives after immediate CPU writes to live Host mapped memory are
removed (`shadow_copies=0`, `shadow_bytes=0`, and no uncovered-range fallback).
That mapped-memory overwrite was a real hazard, but it is not the complete root
cause of the visible rollback.

The next P0 is no longer another broad shadow-upload A/B. Build an exact
per-present frame association across these boundaries:

    DXVK frame and decoded view/projection state
      -> Guest VkQueueSubmit sequence and command-buffer generation
      -> renderer execution sequence and exact dynamic-buffer generations
      -> source swapchain image generation
      -> NCP copy-complete generation
      -> displayed NativeImage timestamp

For each presented frame, record all Heaven pass-0 dynamic uniform bindings,
especially slots 163 and 164, rather than one hash. A valid record must prove
that one draw consumed a coherent set of generations and that the source image
was not reused before the NCP copy fence completed. The result must distinguish:

1. Camera/view data itself moves backward.
2. One draw combines constant blocks from different Guest generations.
3. Renderer executes an older buffer generation for a newer Guest submit.
4. The Vulkan source image is overwritten or reused before the present copy
   completes.
5. The final SurfaceQueue re-publishes an old image (already less likely from
   monotonic serial/timestamp evidence).

No performance fast path, frame dropping, or 60-minute gate may proceed until
this trace identifies the boundary of the first generation regression and both
Heaven continuous observation and Cube `angleRegressions=0` pass on the exact
same archived HAP.

## 22. 2026-07-27 descriptor-update serialization A/B

The first descriptor-serialization artifact cannot be evaluated:

    HAP SHA-256:
      fad3600f19ba0054f74da3f2b44d30776ff67aebe54d815618d5ae49d4d
    archive:
      D:\MyProject\winehua-logs\manual\heaven-descriptor-serialized-20260727-0038
    status:
      INVALID-profile-selector-not-propagated

The NAPI process received the requested profile, but NativeChildProcess does
not reliably inherit arbitrary process environment. The renderer therefore
started with `VKR_WINEHUA_DESCRIPTOR_UPDATE_SERIALIZE=0`, and no
`WineHua descriptor update queue wait` record appeared. Installing an artifact
is not proof that its controlled variable was active.

The selector is now carried through the existing explicit graphics-broker IPC:

    SetHostShadowProfile
      -> VKR_WINEHUA_SHADOW_TRACE=inline-gpu-upload-descriptor-serialized
      -> NativeChildProcess config.shadowTrace
      -> VKR_WINEHUA_DESCRIPTOR_UPDATE_SERIALIZE=1

The replacement artifact was built successfully and archived before install:

    HAP SHA-256:
      aa92506e7d0579fa29470dacb046703df949eb4d8f1d61bed90480273900b3e9
    wine-data SHA-256:
      9d791950b65302d4cdb2b67968bff4aecd16a151523e2279b2eb0f8493b67c4e
    archive:
      D:\MyProject\winehua-logs\manual\heaven-descriptor-serialized-valid-20260727-005103
    status:
      TESTING-runtime-proven-user-verdict-pending

This is a diagnostic A/B only. It keeps the inline queue-ordered upload path
and calls `QueueWaitIdle` on every Host queue immediately before each Host
`vkUpdateDescriptorSets`. It must prove all three runtime facts before any
visual verdict is accepted:

    VKR_WINEHUA_DESCRIPTOR_UPDATE_SERIALIZE=1
    WineHua descriptor update queue wait count=...
    VKR_WINEHUA_GPU_UPLOAD_SERIALIZE=0

If continuous Heaven rollback disappears, the product fix is descriptor-set
generation/lifetime retirement, not permanent global queue-idle waits. If it
remains, descriptor-update overlap is excluded and the next trace must join the
executed dynamic-buffer generation to the source swapchain image and final
present serial.

From this point onward, every visual milestone is processed in this order:

1. Archive the actual signed HAP, hashes, source/submodule state, and profile.
2. Install only that archived hash and prove the controlled variable in logs.
3. Record automatic evidence and the user's continuous visual verdict.
4. Update this memo and make a local checkpoint commit before replacing it.

Sparse screenshots, unique Guest hashes, and successful installation never
override a missing runtime proof or a continuous user-observed rollback.

The physical-device run has now proven the controlled variable. NAPI logged
`selector=inline-gpu-upload-descriptor-serialized` and
`descriptor_serialize=1`; NCP logged inline upload enabled, upload serialization
disabled, and descriptor serialization enabled. The Host log contains repeated
successful queue waits and still reports `shadow_copies=0` / `shadow_bytes=0`.
The visual result remains pending the user's continuous full-rate observation.
