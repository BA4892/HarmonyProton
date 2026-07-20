#include "wine_env.h"
#include "wine_constants.h"
#include "audio_broker.h"
#include "audio_ipc_protocol.h"
#include "graphics_broker.h"
#include "wayland_server.h"

#include <unistd.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#undef LOG_TAG
#define LOG_TAG "WL_NAPI"
#include <hilog/log.h>

int CreateAudioBootstrapFd(const std::string& runtimeDir) {
    if (!winehua::AudioBroker::GetInstance().EnsureStarted(runtimeDir)) {
        OH_LOG_ERROR(LOG_APP, "[AudioBroker] failed to start for runtimeDir=%{public}s", runtimeDir.c_str());
        return -1;
    }
    int fd = winehua::AudioBroker::GetInstance().CreateBootstrapHandle();
    if (fd < 0) {
        OH_LOG_ERROR(LOG_APP, "[AudioBroker] failed to create bootstrap FD for runtimeDir=%{public}s", runtimeDir.c_str());
        return -1;
    }
    OH_LOG_INFO(LOG_APP, "[AudioBroker] bootstrap ready runtimeDir=%{public}s", runtimeDir.c_str());
    return fd;
}

std::vector<std::string> BuildWineEnv(const std::string& sockDir,
                                      const std::string& sockName,
                                      const std::string& libPath,
                                      const std::string& binDir,
                                      int audioBootstrapFd,
                                      const std::string& homeDir,
                                      const std::string& prefixDir) {
    std::string shareDir = binDir + "/../share";
    std::string xkbDir = shareDir + "/X11/xkb";
    std::string midiSoundfontPath = binDir + "/../audio/winehua-gm.sf2";
    std::string runtimeLibPath = binDir + ":" + binDir + "/x86_64-unix:" + binDir + "/../lib/x86_64";
    winehua::GraphicsBackendState graphicsState = winehua::GraphicsBroker::GetInstance().GetState();
    std::string guestReceiverLibDir;
    bool useGuestReceiverRuntime = graphicsState.active == winehua::GraphicsBackend::Virgl;

    if (useGuestReceiverRuntime && graphicsState.guestReceiverPresent && !graphicsState.guestReceiverRuntimeDir.empty()) {
        guestReceiverLibDir = graphicsState.guestReceiverRuntimeDir + "/lib";
        if (access(guestReceiverLibDir.c_str(), F_OK) == 0) {
            runtimeLibPath = guestReceiverLibDir + ":" + runtimeLibPath;
        }
    }

    std::vector<std::string> env = {
        "XDG_RUNTIME_DIR=" + sockDir,
        "WAYLAND_DISPLAY=" + sockName,
        "HOME=" + homeDir,
        "WINEPREFIX=" + (prefixDir.empty() ? std::string(WINE_PREFIX) : prefixDir),
        "WINEDATADIR=" + shareDir + "/wine",
        "WINEDLLDIR=" + binDir + "/x86_64-unix",
        "WINEDLLDIR0=" + binDir + "/x86_64-windows",
        "WINEDLLDIR1=" + binDir + "/i386-windows",
        "WINEDLLDIR2=" + binDir,
        "WINEDLLPATH=" + binDir + "/x86_64-windows:" + binDir + "/i386-windows:" + binDir,
        "WINEDEBUG=-all",
        "LANG=zh_CN.UTF-8",
        "XKB_CONFIG_ROOT=" + xkbDir,
        "PATH=/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:" + binDir + "/x86_64-windows:" + binDir + "/i386-windows:" + binDir,
        "TMPDIR=" WINE_TMPDIR,
        "MIDI_SOUNDFONT_PATH=" + midiSoundfontPath,
    };
    AppendBox64PerfStrings(env);
#ifdef __aarch64__
    env.push_back("LD_LIBRARY_PATH=" + libPath);
    env.push_back("BOX64_LD_LIBRARY_PATH=" + runtimeLibPath);
#else
    env.push_back("LD_LIBRARY_PATH=" + runtimeLibPath);
#endif
    if (audioBootstrapFd >= 0) {
        env.push_back("WINE_OHOS_AUDIO_ENABLE=1");
        env.push_back("WINE_OHOS_AUDIO_BOOTSTRAP_FD=" + std::to_string(audioBootstrapFd));
        env.push_back("WINE_OHOS_AUDIO_PROTOCOL_VERSION=" + std::to_string(WINEHUA_AUDIO_PROTOCOL_VERSION));
    }
    winehua::GraphicsBroker::GetInstance().SetWineRuntimeBinaryDir(binDir);
    // 告知 winewayland.drv 当前是桌面模式还是独立窗口模式
    env.push_back(std::string("WINEHUA_DESKTOP_MODE=") +
                  (WaylandServer::GetInstance()->IsDesktopMode() ? "1" : "0"));
    winehua::GraphicsBroker::GetInstance().AppendWineEnv(env);

    OH_LOG_INFO(LOG_APP,
                "[WineEnv] backend=%{public}s guestMode=%{public}s guestLib=%{public}s runtimeLibPath=%{public}s",
                winehua::GraphicsBroker::BackendName(graphicsState.active),
                graphicsState.guestReceiverMode.empty() ? "stock-egl" : graphicsState.guestReceiverMode.c_str(),
                guestReceiverLibDir.empty() ? "(none)" : guestReceiverLibDir.c_str(),
                runtimeLibPath.c_str());
    return env;
}

static void UpsertEnvLine(std::vector<std::string>& env, const std::string& line)
{
    const size_t sep = line.find('=');
    if (sep == std::string::npos || sep == 0) return;
    const std::string key = line.substr(0, sep);
    for (std::string& existing : env) {
        if (existing.compare(0, key.size(), key) == 0 &&
            existing.size() > key.size() && existing[key.size()] == '=') {
            existing = line;
            return;
        }
    }
    env.push_back(line);
}

void AppendD3dBackendEnv(std::vector<std::string>& env,
                         const std::string& d3dBackend,
                         const std::string& binDir)
{
    if (d3dBackend.rfind("dxvk_", 0) != 0) return;

    std::string profile = d3dBackend.substr(strlen("dxvk_"));
    if (profile.empty()) profile = "legacy";
    const std::string overlayRoot = std::string(WINE_RUNTIME_ROOT) +
        "/smoke/dxvk/" + profile;
    const std::string overlay64 = overlayRoot + "/x64";
    const std::string overlay86 = overlayRoot + "/x86";
    const std::string guestVulkanRoot = binDir + "/guest_vulkan";
    const std::string guestVulkanLib = guestVulkanRoot + "/lib";
    const std::string guestVulkanIcd = guestVulkanRoot +
        "/share/vulkan/icd.d/venus_icd.x86_64.json";
    const std::string box64LibraryPath = guestVulkanLib + ":" +
        binDir + "/guest_gfx/lib:" + binDir + ":" +
        binDir + "/x86_64-unix:" + std::string(WINE_RUNTIME_ROOT) + "/lib/x86_64";
    const std::string wineDllPath = overlay64 + ":" + overlay86 + ":" +
        binDir + "/x86_64-windows:" + binDir + "/i386-windows:" + binDir;

    const std::vector<std::string> managed = {
        "WINEHUA_D3D_BACKEND=" + d3dBackend,
        "WINEHUA_DXVK_ROOT=" + overlayRoot,
        "WINEHUA_DXVK_PROFILE=" + profile,
        "WINEHUA_DXVK_VERSION=1.10.3",
        "WINEHUA_DXVK_RELAXED_FEATURES=1",
        "WINEHUA_VULKAN_RUNTIME=1",
        "WINEHUA_VULKAN_LOADER_ARCH=x86_64",
        "WINEHUA_VENUS_ICD_ARCH=x86_64",
        "USE_LIBBOX64=1",
        "BOX64_LD_LIBRARY_PATH=" + box64LibraryPath,
        "BOX64_EMULATED_LIBS=libvulkan.so:libvulkan.so.1:"
            "libEGL.so:libEGL.so.1:libGLESv2.so:libGLESv2.so.2:"
            "libGLESv1_CM.so:libGLESv1_CM.so.1:libGL.so:libGL.so.1:"
            "libwayland-client.so:libwayland-client.so.0:libwayland-server.so:"
            "libwayland-server.so.0:libwayland-egl.so:libwayland-egl.so.1:"
            "libdrm.so:libdrm.so.2:libffi.so:libffi.so.8",
        "VK_DRIVER_FILES=" + guestVulkanIcd,
        "VK_ICD_FILENAMES=" + guestVulkanIcd,
        "VN_DEBUG=vtest",
        /* Host GPU writes to Venus feedback buffers are not automatically
         * visible through WineHua's explicit Guest/Host shadow mapping.
         * Query the real Host objects instead of polling stale Guest words. */
        "VN_PERF=no_fence_feedback,no_query_feedback",
        "WINEDLLOVERRIDES=d3d11=n;dxgi=n",
        "DXVK_WINEHUA_COMMAND_QUERY_RESET=1",
        "DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED=1",
        "VN_WINEHUA_REMOTE_MEMORY_SYNC=1",
        "WINEDLLPATH=" + wineDllPath,
        "WINEDLLDIR0=" + overlay64,
        "WINEDLLDIR1=" + overlay86,
    };
    for (const std::string& line : managed) UpsertEnvLine(env, line);
}

static bool ShouldSerializeEntryParamEnv(const std::string& envLine) {
    return envLine.rfind("WINE_OHOS_AUDIO_ENABLE=", 0) != 0 &&
           envLine.rfind("WINE_OHOS_AUDIO_BOOTSTRAP_FD=", 0) != 0 &&
           envLine.rfind("WINE_OHOS_AUDIO_PROTOCOL_VERSION=", 0) != 0 &&
           envLine.rfind("WINESERVERSOCKET=", 0) != 0;
}

static std::string EnvKey(const std::string& envLine) {
    size_t sep = envLine.find('=');
    return sep == std::string::npos ? envLine : envLine.substr(0, sep);
}

static bool IsBrokerSessionAuthoritativeKey(const std::string& key) {
    // Explorer may start before VirGL is ready. Replace its early Box64 path
    // with the finalized path, where guest graphics libraries are a fallback.
    return key == "BOX64_LD_LIBRARY_PATH";
}

size_t AppendMissingEntryParamsEnvOverrides(std::string& entryParams,
                                            const std::vector<std::string>& env) {
    std::unordered_set<std::string> existingKeys;
    size_t pos = 0;

    while ((pos = entryParams.find("|__env=", pos)) != std::string::npos) {
        pos += strlen("|__env=");
        size_t end = entryParams.find('|', pos);
        std::string key = EnvKey(entryParams.substr(pos, end == std::string::npos
                                                          ? std::string::npos
                                                          : end - pos));
        if (!key.empty()) existingKeys.insert(std::move(key));
        if (end == std::string::npos) break;
        pos = end;
    }

    size_t appended = 0;
    for (const std::string& envLine : env) {
        if (!ShouldSerializeEntryParamEnv(envLine) ||
            envLine.find('|') != std::string::npos ||
            envLine.find('\n') != std::string::npos)
            continue;

        std::string key = EnvKey(envLine);
        if (key.empty() ||
            (existingKeys.count(key) && !IsBrokerSessionAuthoritativeKey(key)))
            continue;
        entryParams += "|__env=";
        entryParams += envLine;
        existingKeys.insert(std::move(key));
        ++appended;
    }
    return appended;
}

void LogGraphicsBackendStateForLaunch(const char* tag) {
    winehua::GraphicsBackendState state = winehua::GraphicsBroker::GetInstance().GetState();
    OH_LOG_INFO(LOG_APP,
                "[%{public}s] graphics requested=%{public}s active=%{public}s runtimeReady=%{public}s "
                "guestReceiver=%{public}s(%{public}s) virglSocketReady=%{public}s virglLibraryPresent=%{public}s",
                tag,
                winehua::GraphicsBroker::BackendName(state.requested),
                winehua::GraphicsBroker::BackendName(state.active),
                state.runtimeReady ? "true" : "false",
                state.guestReceiverPresent ? "true" : "false",
                state.guestReceiverMode.empty() ? "stock-egl" : state.guestReceiverMode.c_str(),
                state.virglSocketReady ? "true" : "false",
                state.virglLibraryPresent ? "true" : "false");
    if (!state.lastError.empty())
        OH_LOG_WARN(LOG_APP, "[%{public}s] graphics note: %{public}s", tag, state.lastError.c_str());
}
