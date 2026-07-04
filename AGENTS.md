# AGENTS.md — WineHua (Wine on HarmonyOS)

## Build

**Always use the top-level `make` or `build.sh`. Never `make -C build/wine-ohos` or `make -C build/wine-native`.** The top-level entrypoint sets `NATIVE_ARCH`, `DEVICE_TYPE`, cross-compilation toolchains, `PAD_CFLAGS`, and runs the full assemble→HAP→sign pipeline via stamp-based incremental builds.

### Primary: `Makefile`

```bash
# ARM64 Pad (current main target)
make NATIVE_ARCH=arm64-v8a DEVICE_TYPE=pad

# ARM64 Pad — HAP only (skip Wine/deps when only ArkTS or entry/src/main/cpp/ changed)
make NATIVE_ARCH=arm64-v8a DEVICE_TYPE=pad hap

# x86_64 PC
make NATIVE_ARCH=x86_64 DEVICE_TYPE=pc
```

### Convenience: `build.sh`

```bash
bash build.sh pad arm64          # full Pad build
bash build.sh pad-hap arm64      # Pad HAP only (skip Wine/deps)
bash build.sh pad-deploy <ip>    # push + install Pad HAP
bash build.sh quick <ip> arm64   # PC: assemble→hnp→hap→deploy
```

Single modules: `make deps | wine | box64 | native | assemble | hap`. Per-arch: `make native-x86_64`, `make native-arm64-v8a`.

## Two device modes

| | PC (`DEVICE_TYPE=pc`) | Pad (`DEVICE_TYPE=pad`) |
|---|---|---|
| Process model | execve + HNP packages | fork-only, no execve, NCP appspawn |
| Wine data | HNP bundle | rawfile zip, extracted at runtime |
| Box64 | executable | .so, dlopen'd by `wine_child.so` |
| PAD_MODE | not defined | `-DPAD_MODE` (exported via `PAD_CFLAGS` from `scripts/env.sh`) |

## Source layout

- `entry/src/main/cpp/` — C++17 native code: Wayland compositor, EGL renderer, input manager, Wine process lifecycle, NAPI bridge. 16 source files. Builds `libentry.so` (NAPI) + `libwine_child.so` (NCP child).
- `entry/src/main/ets/` — ArkTS UI: pages (Index, WineWindow, DesktopWindow), WineWindowManager, key/mouse maps.
- `thirdparty/` — 12 git submodules including forked wine (winehua/wine), forked box64 (winehua/box64), and stock wayland/freetype/libxkbcommon/etc.
- `scripts/` — 16 shell scripts. `env.sh` is the shared environment for all build scripts.
- `docs/` — architecture, build guide, current status docs.

### Key C++ files

| File | Purpose |
|---|---|
| `napi_init.cpp` | 27 NAPI functions: startServer, launchClient, runWineExe, sendPointerEvent, sendKeyEvent, etc. |
| `wine_child.cpp` | NCP child process entries: `Main()` and `WineserverMain()` |
| `wine_launch.cpp` | Background thread: starts wineserver → wineboot → explorer desktop |
| `wayland_server.cpp` | Embedded Wayland compositor (wl_compositor, xdg_shell, wl_seat) |
| `egl_renderer.cpp` | EGL/GLES render loop for XComponent surfaces |

## Submodule repos

Wine and Box64 use **forked** repos:
- `thirdparty/wine` — `github.com/winehua/wine` (branch `master`)
- `thirdparty/box64` — `github.com/winehua/box64` (branch `main`)
- `thirdparty/virglrenderer` — `github.com/winehua/virglrenderer`
- `thirdparty/mesa` — `gitee.com/openharmony/third_party_mesa3d` (branch `OpenHarmony-6.0-Beta1`)

## Build environment

Requires OHOS SDK at `/apps/harmony/sdk/default/openharmony` and Command Line Tools at `$TOOL_HOME` (default `/apps/harmony`). A Dockerfile (`Dockerfile`) provides a reproducible Ubuntu 26.04 build environment.

`build-profile.json5` is **gitignored**; copy `build-profile.json5.example` to `build-profile.json5` in a working checkout.

## Stamp-based incremental builds

Build stamps are at `build/.stamps/`. Each module (deps, wine, box64, native, assemble) uses `find -newer` to detect source changes and skip up-to-date modules. Stamps are architecture+device-type specific (e.g., `build/.stamps/arm64-v8a/assemble-pad`).

Wine sentinel: `build/wine-native/tools/winegcc/winegcc` must exist for Wine to be considered built.

## Pad deployment

Current debug Pad device: `hdc -t 192.168.1.6:38823`. After changing Wine source, a full uninstall+reinstall is required (clear `.wine` prefix). After ArkTS/C++ changes only, `aa force-stop` + `bm install` + `aa start` is sufficient. See `.claude/rules/build-and-log.md` for full commands.

## Logging

All Wine stderr is captured to file at `/data/app/el2/100/base/app.hackeris.winehua/temp/wine_stderr_YYYYMMDD.log` and forwarded to hilog with tag `WineChild-stderr`. Key hilog tags for each subsystem are documented in `.claude/rules/build-and-log.md`.

Wine debug channels are controlled by `WINEDEBUG` (default `-all`). To enable specific channels, modify `wine_env.cpp`; note that `TRACE()` may be optimized out in release builds — use `fprintf(stderr, ...)` instead.

## Tests

Minimal. HarmonyOS Hypium framework, trivial asserts only. No CI/CD configured.

## Critical constraints

- **HarmonyOS sandbox prohibits `symlink()`** — dosdevices symlinks use hardcoded `drive_c` fallback paths.
- **Pad devices have noexec filesystem** — executable segments use anonymous `mmap` + `pread` instead of file-backed mmap.
- **Pad devices have no `execve`** — child processes are spawned via `OH_Ability_StartNativeChildProcess("libwine_child.so:Main")`.
- **Input event pipeline** goes ArkTS → NAPI → Compositor → Wine, with hilog tags tracing the full path (CLICK-PIPE, KBD-PIPE, WL_NAPI, WL_Input).
- **Multi-window rendering** uses `XComponentController` callback-obtained `surfaceId` values, not statically assigned IDs, to avoid conflicts.
- `build-profile.json5` is gitignored — must be copied from `.example` for new checkouts.

## Reference documents

- `.claude/rules/build-and-log.md` — comprehensive build commands, deploy, hilog tags, and input event debugging workflow
- `docs/ARCHITECTURE.md` — Wine internal layers, Box64 integration, signal handling
- `docs/BUILD_GUIDE.md` — full build environment setup
- `docs/CURRENT_STATUS.md` — feature status and fix checklist
