# WineHua DXVK Modern Upgrade Readiness

> Updated: 2026-07-30

> Purpose: record capability evidence and upgrade gates for a future DXVK 2.x
> or VKD3D investigation. This is a planning and qualification memo, not
> authorization to replace the current product runtime.

## 1. Current product decision

The Phase 2 product runtime remains the WineHua DXVK 1.10.3 fork.

```text
DXVK Legacy 1.10.3
  -> Wine Vulkan
  -> x86_64 Mesa Venus
  -> virglrenderer Venus
  -> Host Harmony Vulkan
  -> Venus BrokerPresent
```

The immediate priority is compatibility, visual correctness, regression
coverage, and long-run stability on this path. Do not replace its managed DLLs
with an upstream DXVK 2.x build. A future Modern runtime must be packaged as a
separate manifest profile, selected before process startup, and retain this
fallback order:

```text
DXVK_MODERN -> DXVK_LEGACY -> WineD3D
```

Switching DLLs inside a running Wine process is invalid. A failed Modern test
is recorded as a failure even if a separate Legacy restart succeeds.

## 2. Capability evidence captured on 2026-07-30

The device identity below is the Host Vulkan device name. The Guest identity is
the corresponding Venus adapter.

| Capability | 9010 / Maleoon 910 | 9020 / Maleoon 920 | Upgrade interpretation |
| --- | --- | --- | --- |
| Host API version | 1.2.275 | 1.3.309 | Only 920 reaches the DXVK 2.x API baseline. |
| Venus Guest API version | 1.2.275 | 1.3.269 | 920 exposes Vulkan 1.3 to Wine/DXVK. |
| `VK_EXT_robustness2` | no | yes | DXVK 2.x requires robust buffer access and null descriptors. |
| `synchronization2` | yes | yes | Already available on both; Modern uses the newer submission path. |
| `dynamicRendering` | no | yes | 920 can execute the Modern render path. |
| `maintenance4` / `maintenance5` | no | yes | Useful to newer DXVK, but not sufficient alone. |
| `VK_EXT_transform_feedback` | no | yes, `geometryStreams=0` | Ordinary stream output is possible; multi-stream remains incomplete. |
| `VK_EXT_extended_dynamic_state` | no | yes | Current Legacy DXVK already enables it on 920. |
| `VK_EXT_vertex_attribute_divisor` | no | yes | Current Legacy DXVK already enables it on 920. |
| `VK_EXT_shader_demote_to_helper_invocation` | no | yes | Current Legacy DXVK already enables it on 920. |
| Descriptor indexing | Host yes, Venus no | Host yes, Venus no | Unusable until Venus exposes it. |
| ETC2 / ASTC | Host yes, Venus no | Host yes, Venus no | Not a direct replacement for DXGI BC data. |
| BC1-BC7 | no | no | WineHua BC decode/emulation remains necessary. |
| Buffer device address | not qualified | Venus reports no | Blocks current DXVK 2.7+ profile requirements. |

Primary evidence:

```text
D:\MyProject\winehua-logs\automation\capabilities-pad-910-20260730\phase2-20260730-130554\capability-matrix.json
D:\MyProject\winehua-logs\automation\capabilities-phone-920-title-20260730\phase2-20260730-131706\capability-matrix.json
```

The 920 DXVK runtime log proves that the installed Legacy runtime enables
`VK_EXT_robustness2`, `VK_EXT_extended_dynamic_state`,
`VK_EXT_shader_demote_to_helper_invocation`, `VK_EXT_transform_feedback`, and
`VK_EXT_vertex_attribute_divisor`. The 910 log shows these as unavailable. This
is capability-driven behavior, not a device-name performance special case.

## 3. What Vulkan 1.3 changes today

Vulkan 1.3 does not by itself increase frame rate. A feature must be exposed by
Host Vulkan, passed through Venus, enabled by DXVK, and exercised by the game.

On 920, Legacy DXVK already benefits where it can from null descriptors,
extended dynamic state, shader demote, transform feedback, and vertex divisor.
These can reduce state churn or improve compatibility for particular games, but
they do not remove Wine, Box64, Venus transport, shadow-memory, or Host-driver
costs.

Modern DXVK 2.x additionally uses `vkQueueSubmit2` and dynamic rendering. This
may reduce CPU/front-end overhead and render-pass management work. It is not
expected to transform a Host-GPU-bound workload such as tessellation-heavy
Heaven. `VK_EXT_graphics_pipeline_library` is not currently exposed by the 920
Venus adapter, so its major shader-stutter benefit must not be assumed.

The BrokerPresent GPU copy is not the reason different GPUs would have the same
performance. Existing Heaven profiling places that copy at roughly 0.06-0.19
ms. Low-quality Heaven scenes retain meaningful shared front-end cost, while
tessellation-heavy scenes are dominated by Host GPU completion. A stronger Host
GPU should therefore help heavy scenes, but gains will not be linear in light
scenes and can be affected by resolution and sustained thermal limits.

## 4. Why stock DXVK 2.x cannot be dropped in

DXVK 2.0 through 2.6.x require a Vulkan 1.3 driver and
`VK_EXT_robustness2`. The 920 Guest satisfies those core requirements; the 910
Guest satisfies neither and must remain on Legacy or WineD3D.

However, upstream DXVK 2.6.2 D3D11 baseline requests these desktop features
without the WineHua fallbacks used by the Legacy fork:

```text
dualSrcBlend
multiViewport
textureCompressionBC
transformFeedback + geometryStreams
```

The current 920 Venus adapter reports:

```text
dualSrcBlend          = 0
multiViewport         = 0
textureCompressionBC  = 0
transformFeedback     = 1
geometryStreams       = 0
```

An unmodified upstream 2.0 or 2.6.x DLL is therefore expected to reject the
D3D11 feature baseline or expose an unsafe partial device. Vulkan 1.3 does not
change that conclusion.

DXVK 2.7.1 and later are not the initial target. The published 2.7 profile
requires descriptor indexing, runtime descriptor arrays, update-after-bind,
maintenance5, and buffer device address. Current 920 Venus exposes neither
descriptor indexing nor buffer device address. Re-evaluate only after a new
Guest capability capture proves all required features, limits, and extensions.

## 5. Required WineHua compatibility inventory

A Modern branch is a forward-port, not a DLL swap and not a blind cherry-pick
series. Before it can create a D3D11 device on current Maleoon/Venus, inventory
and port the production portions of the Legacy compatibility implementation:

1. Relaxed D3D11 capability policy that never claims unsupported native Vulkan
   features, paired with explicit emulation or `UNSUPPORTED`.
2. BC1-BC7 upload decode and uncompressed backing-image/view handling.
3. Qualified dual-source blend fallback.
4. Qualified RGBA8 SNORM render-target fallback.
5. Custom-border, sampler, format-view, depth comparison, and Cube/CubeArray
   Maleoon quirks documented in `thirdparty/dxvk/WINEHUA_FORK.md`.
6. Mapped-memory flush ownership and Venus shadow synchronization contract.
7. Bool sampled-descriptor specialization policy and its capability/adapter
   quirk controls.

Diagnostics may be redesigned for the Modern code base. Correctness fixes,
semantic emulation, runtime manifest integration, and smoke coverage must not
be omitted merely because their original commits do not apply cleanly.

## 6. Deferred Modern DXVK plan

When Legacy is mature, create a separate WineHua fork branch from a pinned
upstream tag. Do not rebase or rewrite `dxvk-legacy-1.10.3`.

1. Start with a DXVK 2.0 startup probe to validate the Vulkan 1.3,
   `vkQueueSubmit2`, and dynamic-rendering transport contract on 920.
2. Use DXVK 2.6.2 as the first realistic Modern performance candidate if the
   startup probe succeeds. It predates the 2.7 descriptor-indexing baseline.
3. Forward-port the compatibility inventory in section 5 and build x64 and x86
   DLLs into `files/wine/dxvk/modern/<arch>/`.
4. Add a manifest capability gate. It must check Guest Venus capability, not
   only Host Vulkan or a GPU model string.
5. Require x64/x86 Vulkan and D3D11 smoke, descriptor/subresource/BC/depth
   matrices, visible Cube monotonicity, and no WineD3D fallback.
6. Run fixed-setting Legacy versus Modern A/B on Heaven, ComputeMark, Tomb
   Raider, and other real workloads. Record DLL identity, frame percentiles,
   Host CPU time, completion wait, shadow bytes, present cost, output image,
   and crash status.
7. Require clean prefix, three reuse-prefix passes, overwrite-install refresh,
   and a separate 60-minute stability gate before making Modern selectable for
   normal game launch.

The default remains Legacy until a Modern candidate passes all gates. A new
capability hash, Host driver update, Venus update, or Mesa update invalidates
the qualification and requires rerunning the matrix.

## 7. VKD3D boundary

VKD3D is a separate future project, not a consequence of upgrading DXVK.
DXVK Modern qualification proves a D3D9/10/11 translation path only. A VKD3D
phase requires a fresh D3D12 capability matrix, resource-binding and memory
model audit, descriptor-indexing/buffer-device-address review, D3D12 smoke,
and independent real-game stability evidence. Current Venus gaps make it
inappropriate to promise VKD3D support or performance from DXVK 2.x work.

## 8. Decision record

Until this document is explicitly revised after a new qualification run:

```text
Product default:       WineHua DXVK Legacy 1.10.3
Immediate objective:   compatibility fixes and stability
Modern DXVK:           deferred, separate controlled profile
Initial Modern target: DXVK 2.0 probe, then DXVK 2.6.2 candidate
DXVK 2.7+/3.x:         deferred pending Venus descriptor/BDA capability work
VKD3D:                  deferred to an independent D3D12 phase
```
