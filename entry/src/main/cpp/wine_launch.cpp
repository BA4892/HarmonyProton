#include "wine_launch.h"
#include "wine_process.h"
#include "wine_env.h"
#include "wine_constants.h"
#include "wayland_server.h"
#include "audio_ipc_protocol.h"
#include "graphics_broker.h"

#include <unistd.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <string>
#include <strings.h>
#include <thread>
#include <vector>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_NAPI"
#include <hilog/log.h>

#include "broker.h"
#include "wait_utils.h"

#include <AbilityKit/native_child_process.h>

// -- prefix 初始化检测辅助函数 --
static bool FileHasData(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static bool DirExists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool IsWinePrefixInitialized(const std::string& prefixDir) {
    const std::string prefix = prefixDir.empty() ? WINE_PREFIX : prefixDir;
    return FileHasData((prefix + "/system.reg").c_str()) &&
           FileHasData((prefix + "/user.reg").c_str()) &&
           DirExists((prefix + "/drive_c/windows/system32").c_str()) &&
           DirExists((prefix + "/drive_c/windows/temp").c_str()) &&
           DirExists((prefix + "/drive_c/users").c_str());
}

bool IsWinePrefixInitialized() {
    return IsWinePrefixInitialized(WINE_PREFIX);
}

// fork 模式下子进程退出先变僵尸、/proc/<pid> 不消失（NCP 模式由 appspawn 立即 reap）。
// 存活检测必须识别僵尸，否则 wineboot 等待会白等到 kWinebootHangMs 超时。
static bool IsProcessAliveNotZombie(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    FILE* f = fopen(path, "r");
    if (!f) return false;                       // /proc 消失 = 已退出
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    char* rp = strrchr(buf, ')');               // state 字段在最后一个 ')' 之后
    return !(rp && rp[2] == 'Z');               // 僵尸 = 已退出
}

// -- WoW64 syswow64 预填充辅助 --
static bool EnsureDir(const std::string& path, mode_t mode)
{
    if (DirExists(path.c_str())) return true;
    if (mkdir(path.c_str(), mode) == 0 || errno == EEXIST) return DirExists(path.c_str());
    OH_LOG_ERROR(LOG_APP, "[Launch-Async] mkdir %{public}s failed: %{public}s",
                 path.c_str(), strerror(errno));
    return false;
}

static bool EnsureDirRecursive(const std::string& path, mode_t mode)
{
    if (path.empty() || path == "/") return true;
    if (DirExists(path.c_str())) return true;

    size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0)
    {
        if (!EnsureDirRecursive(path.substr(0, slash), mode)) return false;
    }
    return EnsureDir(path, mode);
}

static bool EnsureExternalPePrefixSkeleton(const std::string& prefixDir)
{
    // The external-PE runtime resolves 64-bit Windows binaries from
    // x86_64-windows instead of copying them into drive_c.  wineboot therefore
    // does not necessarily materialize directories which Windows services and
    // diagnostics still use as working/output directories.
    static const char* const suffixes[] = {
        "/drive_c/windows/system32",
        "/drive_c/windows/system32/drivers",
        "/drive_c/windows/system32/spool",
        "/drive_c/windows/system32/tasks",
        "/drive_c/windows/temp",
    };

    bool ok = true;
    for (const char* suffix : suffixes)
        ok = EnsureDirRecursive(prefixDir + suffix, 0777) && ok;

    OH_LOG_INFO(LOG_APP, "[Launch-Async] external-PE prefix skeleton %{public}s",
                ok ? "ready" : "failed");
    return ok;
}

static bool HasRuntimeFileExtension(const char* name)
{
    const char* dot = strrchr(name, '.');
    if (!dot) return false;
    return !strcasecmp(dot, ".dll") ||
           !strcasecmp(dot, ".drv") ||
           !strcasecmp(dot, ".sys") ||
           !strcasecmp(dot, ".exe");
}

static bool CopyFileIfNeeded(const std::string& src, const std::string& dst)
{
    struct stat srcSt;
    struct stat dstSt;
    if (stat(src.c_str(), &srcSt) != 0 || !S_ISREG(srcSt.st_mode)) return false;
    if (stat(dst.c_str(), &dstSt) == 0 && S_ISREG(dstSt.st_mode) &&
        dstSt.st_size == srcSt.st_size && dstSt.st_mtime >= srcSt.st_mtime)
        return true;

    int inFd = open(src.c_str(), O_RDONLY);
    if (inFd < 0)
    {
        OH_LOG_ERROR(LOG_APP, "[Launch-Async] open src %{public}s failed: %{public}s",
                     src.c_str(), strerror(errno));
        return false;
    }

    std::string temporaryTemplate = dst + ".winehua.tmp.XXXXXX";
    std::vector<char> temporary(temporaryTemplate.begin(), temporaryTemplate.end());
    temporary.push_back('\0');
    int outFd = mkstemp(temporary.data());
    if (outFd < 0)
    {
        OH_LOG_ERROR(LOG_APP, "[Launch-Async] create temporary for %{public}s failed: %{public}s",
                     dst.c_str(), strerror(errno));
        close(inFd);
        return false;
    }
    fchmod(outFd, 0666);

    char buffer[64 * 1024];
    bool ok = true;
    ssize_t n;
    while ((n = read(inFd, buffer, sizeof(buffer))) > 0)
    {
        char* p = buffer;
        ssize_t remaining = n;
        while (remaining > 0)
        {
            ssize_t w = write(outFd, p, remaining);
            if (w < 0)
            {
                ok = false;
                OH_LOG_ERROR(LOG_APP, "[Launch-Async] write dst %{public}s failed: %{public}s",
                             dst.c_str(), strerror(errno));
                break;
            }
            p += w;
            remaining -= w;
        }
        if (!ok) break;
    }
    if (n < 0)
    {
        ok = false;
        OH_LOG_ERROR(LOG_APP, "[Launch-Async] read src %{public}s failed: %{public}s",
                     src.c_str(), strerror(errno));
    }

    if (ok && fsync(outFd) != 0)
    {
        ok = false;
        OH_LOG_ERROR(LOG_APP, "[Launch-Async] fsync dst %{public}s failed: %{public}s",
                     dst.c_str(), strerror(errno));
    }
    close(outFd);
    close(inFd);
    if (ok && rename(temporary.data(), dst.c_str()) != 0)
    {
        ok = false;
        OH_LOG_ERROR(LOG_APP, "[Launch-Async] replace dst %{public}s failed: %{public}s",
                     dst.c_str(), strerror(errno));
    }
    if (!ok) unlink(temporary.data());
    return ok;
}

static bool EnsureWow64Files(const std::string& binDir, const std::string& prefixDir)
{
    const std::string srcDir = binDir + "/i386-windows";
    const std::string dstDir = prefixDir + "/drive_c/windows/syswow64";

    DIR* src = opendir(srcDir.c_str());
    if (!src)
    {
        OH_LOG_ERROR(LOG_APP, "[Launch-Async] wow64 source missing %{public}s: %{public}s",
                     srcDir.c_str(), strerror(errno));
        return false;
    }
    if (!EnsureDirRecursive(dstDir, 0777))
    {
        closedir(src);
        return false;
    }

    int total = 0;
    int copied = 0;
    int failed = 0;
    while (dirent* entry = readdir(src))
    {
        if (entry->d_name[0] == '.' || !HasRuntimeFileExtension(entry->d_name)) continue;
        total++;
        std::string srcPath = srcDir + "/" + entry->d_name;
        std::string dstPath = dstDir + "/" + entry->d_name;
        if (CopyFileIfNeeded(srcPath, dstPath)) copied++;
        else failed++;
    }
    closedir(src);

    OH_LOG_INFO(LOG_APP, "[Launch-Async] wow64 syswow64 total=%{public}d ok=%{public}d failed=%{public}d",
                total, copied, failed);
    return total > 0 && failed == 0;
}
static bool IsWineserverSocketReady(const std::string& prefix) {
    char sockDir[512];
    snprintf(sockDir, sizeof(sockDir), "%s/.wineserver", prefix.c_str());
    DIR* d = opendir(sockDir);
    if (!d) return false;
    bool found = false;
    struct dirent* de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char sockPath[1024];
        snprintf(sockPath, sizeof(sockPath), "%s/%s/socket", sockDir, de->d_name);
        struct stat st;
        if (stat(sockPath, &st) == 0 && S_ISSOCK(st.st_mode)) { found = true; break; }
    }
    closedir(d);
    return found;
}

static std::string FindLaunchEnvironmentValue(const LaunchParams& params,
                                              const char* key)
{
    const std::string prefix = std::string(key) + "=";
    for (auto it = params.envStrs.rbegin(); it != params.envStrs.rend(); ++it) {
        if (it->rfind(prefix, 0) == 0)
            return it->substr(prefix.size());
    }
    return {};
}

static void AppendStableDesktopDxvkEnv(std::vector<std::string>& env,
                                       const LaunchParams& params)
{
    if (params.d3dBackend.rfind("dxvk_", 0) != 0) return;

    /* SetHostShadowProfile carries the selected diagnostic profile through
     * the host-side broker environment before Explorer is launched.  Keep
     * the desktop descendants on that explicit profile instead of replacing
     * it with the product default below. */
    const char* shadowTrace = getenv("VKR_WINEHUA_SHADOW_TRACE");
    const bool guestPerf = shadowTrace && !strcmp(shadowTrace, "perf");
    std::string selectedProfile =
        FindLaunchEnvironmentValue(params, "WINEHUA_PERF_PROFILE");
    if (selectedProfile.empty()) {
        if (shadowTrace && !strcmp(shadowTrace, "inline-gpu-upload-frame-assoc-trace"))
            selectedProfile = "shadow-precise-dirty-ring-frame-assoc-trace";
        else if (shadowTrace && !strcmp(shadowTrace, "present-image-trace"))
            selectedProfile = "shadow-precise-dirty-ring-present-image-trace";
        else if (shadowTrace && !strcmp(shadowTrace, "inline-gpu-upload-descriptor-serialized"))
            selectedProfile = "shadow-precise-dirty-ring-inline-upload-descriptor-serialized";
        else if (shadowTrace && !strcmp(shadowTrace, "inline-gpu-upload-serialized"))
            selectedProfile = "shadow-precise-dirty-ring-inline-upload-serialized";
        else if (shadowTrace && !strcmp(shadowTrace, "inline-gpu-upload-coverage-sort"))
            selectedProfile = "shadow-precise-dirty-ring-inline-upload-coverage-sort";
        else if (shadowTrace && !strcmp(shadowTrace, "inline-gpu-upload"))
            selectedProfile = "shadow-precise-dirty-ring-inline-upload";
        else if (shadowTrace && !strcmp(shadowTrace, "no-gpu-upload-fast"))
            selectedProfile = "shadow-precise-dirty-ring-no-upload-fast";
        else if (shadowTrace && !strcmp(shadowTrace, "no-gpu-upload"))
            selectedProfile = "shadow-precise-dirty-ring-no-upload";
        else
            selectedProfile = guestPerf ? "shadow-precise-strong-ring-perf"
                                        : "shadow-precise-dirty-ring-inline-upload-coverage-sort";
    }

    /* Explorer-launched programs inherit the desktop process environment and
     * bypass Index.d3dLaunchEnvironment(). Keep the product-correct settings
     * here without changing the explicit A/B profiles used by runWineProgram. */
    /* Product sessions retain warnings and errors without formatting DXVK's
     * informational startup stream. Smoke and explicit diagnostics override
     * this through runWineProgram's per-process environment. */
    env.push_back("DXVK_LOG_LEVEL=warn");
    env.push_back("DXVK_LOG_PATH=C:\\windows\\temp");
    env.push_back("BOX64_DYNAREC_WEAKBARRIER=0");
    env.push_back("WINEHUA_PERF_PROFILE=" + selectedProfile);
    env.push_back("DXVK_WINEHUA_PRECISE_SHADOW=1");
    if (selectedProfile == "shadow-precise-dirty-ring-inline-upload-descriptor-serialized") {
        env.push_back("VKR_WINEHUA_DESCRIPTOR_UPDATE_SERIALIZE=1");
    }
    env.push_back("VN_WINEHUA_STRONG_RING_BARRIER=1");
    if (guestPerf) {
        env.push_back("VN_WINEHUA_PERF_SUMMARY=1");
        env.push_back("VN_WINEHUA_PERF_LOG=/storage/Users/currentUser/Download/app.hackeris.winehua/winehua_guest_ring_perf.log");
        /* vn_log uses MESA_LOG_DEBUG.  Raise only the explicit diagnostic
         * profile so the Guest ring summary survives the OHOS logger filter. */
        env.push_back("MESA_LOG_LEVEL=debug");
    }
}

static void PrepareDesktopSessionGraphicsEnv(const LaunchParams& params)
{
    OH_LOG_INFO(LOG_APP, "[Launch-Async] preparing graphics env for child processes");
    auto& gb = winehua::GraphicsBroker::GetInstance();
    gb.SetWineRuntimeBinaryDir(params.winehuaBin);
    gb.SetRequestedBackend(winehua::GraphicsBackend::Virgl);
    gb.SetVulkanPresentMode(params.d3dBackend.rfind("dxvk_", 0) == 0);
    gb.EnsureStarted(params.sockDir);

    winehua::GraphicsBackendState state = gb.GetState();
    if (state.active != winehua::GraphicsBackend::Virgl) {
        OH_LOG_ERROR(LOG_APP,
                     "[Launch-Async] GL env unavailable: requested=%{public}s active=%{public}s error=%{public}s",
                     winehua::GraphicsBroker::BackendName(state.requested),
                     winehua::GraphicsBroker::BackendName(state.active),
                     state.lastError.c_str());
        return;
    }

    std::vector<std::string> env;
    gb.AppendWineEnv(env);
    AppendD3dBackendEnv(env, params.d3dBackend, params.winehuaBin);
    AppendStableDesktopDxvkEnv(env, params);
    /* The broker now receives the finalized environment through the
     * serialized __env entryParams channel. Keep this helper side-effect
     * free so the old broker-global environment path cannot diverge from
     * Explorer and smoke launches. */
    LogGraphicsBackendStateForLaunch("DesktopSession");
}

static void AppendDesktopD3dEntryEnv(std::string& entryParams, const LaunchParams& params)
{
    /* Explorer is launched directly through NCP during desktop bootstrap and
     * therefore does not pass through the process broker. NCP children do not
     * inherit the session environment reliably, so carry the same base
     * Wine/Wayland/VirGL environment that was built for broker launches.
     * Audio and WINESERVERSOCKET are intentionally filtered by
     * AppendMissingEntryParamsEnvOverrides; their per-process descriptors are
     * installed by WineChild after the fd list is applied. */
    std::vector<std::string> env;
    /* Refresh the graphics portion immediately before spawning Explorer.
     * params.envStrs was assembled before wineboot and the VirGL receiver
     * finished starting, so it may still contain a stale SHM fallback. */
    winehua::GraphicsBroker::GetInstance().AppendWineEnv(env);
    AppendD3dBackendEnv(env, params.d3dBackend, params.winehuaBin);
    AppendStableDesktopDxvkEnv(env, params);
    AppendMissingEntryParamsEnvOverrides(entryParams, env);

    /* Fill in the remaining stable baseline values (HOME, prefix, loader
     * paths, etc.) without allowing that early snapshot to replace the fresh
     * graphics state above. */
    AppendMissingEntryParamsEnvOverrides(entryParams, params.envStrs);
}

static bool LaunchPadMode(LaunchParams* p, int audioBootstrapFd,
                          const std::string& serializedEnv) {
    // 通过 fdList 传递 audio bootstrap fd (仅 explorer 需要音频)
    NativeChildProcess_Fd audioFdNode;
    audioFdNode.fdName = const_cast<char*>("wine_audio_bootstrap");
    audioFdNode.fd = audioBootstrapFd;
    audioFdNode.next = nullptr;

    // Prefix registry and user data survive runtime upgrades, while the
    // syswow64 PE files are managed copies. Validate them before wineserver
    // starts so an interrupted prior refresh cannot leave a zero-length DLL.
    if (!EnsureExternalPePrefixSkeleton(p->prefixDir) ||
        !EnsureWow64Files(p->winehuaBin, p->prefixDir)) {
        OH_LOG_ERROR(LOG_APP, "[Launch-Async] external-PE prefix preparation failed");
        if (gStateTsfn)
            napi_call_threadsafe_function(gStateTsfn, strdup("wineboot-failed"), napi_tsfn_blocking);
        return false;
    }

    // -- wineserver via NCP --
    // wineserver 走 WineserverMain 入口, 不解析 __env__, 不需要环境变量
    {
        std::string wsEntryParams = p->homeDir + "|" + p->winehuaBin +
            "|wineserver|-f|-p|--no-auto-close|__env=WINEPREFIX=" + p->prefixDir;
        OH_LOG_INFO(LOG_APP, "[Launch-Async] wineserver args=%{public}s", wsEntryParams.c_str());
        NativeChildProcess_Args wsArgs = {};
        wsArgs.entryParams = const_cast<char*>(wsEntryParams.c_str());
        NativeChildProcess_Options wsOpts = {};
        wsOpts.isolationMode = NCP_ISOLATION_MODE_NORMAL;
        int32_t wsChildPid = -1;
        auto wsRet = OH_Ability_StartNativeChildProcess(
            "libwine_child.so:WineserverMain", wsArgs, wsOpts, &wsChildPid);
        if (wsRet != NCP_NO_ERROR) {
            OH_LOG_ERROR(LOG_APP, "[Launch-Async] wineserver StartNativeChildProcess FAILED ret=%{public}d", (int)wsRet);
            if (gStateTsfn)
                napi_call_threadsafe_function(gStateTsfn, strdup("wineserver-failed"), napi_tsfn_blocking);
            return false;
        }
        OH_LOG_INFO(LOG_APP, "[Launch-Async] wineserver pid=%{public}d (via appspawn)", wsChildPid);
        if (!WaitFor("wineserver socket", [p]() { return IsWineserverSocketReady(p->prefixDir); }, 5000, 100)) {
            OH_LOG_WARN(LOG_APP, "[Launch-Async] wineserver socket not detected, "
                        "wineboot will recover via server_connect retry+start_server");
        }
    }

    if (gStateTsfn)
        napi_call_threadsafe_function(gStateTsfn, strdup("wineboot-starting"), napi_tsfn_blocking);

    gBrokerHomeDir = p->homeDir;
    StartBrokerServer();
    setenv("PROCESSBROKER", WINE_BROKER_SOCKET, 1);

    // -- wineboot --init --
    const std::string initMarker = p->prefixDir + "/.winehua-init-in-progress";
    bool prefixReady = IsWinePrefixInitialized(p->prefixDir)
        && access(initMarker.c_str(), F_OK) != 0;

    if (!prefixReady) {
        OH_LOG_INFO(LOG_APP, "[Launch-Async] prefix not initialized, preparing WoW64 and running wineboot --init...");
        if (FILE* marker = fopen(initMarker.c_str(), "w")) {
            fputs("wineboot\n", marker);
            fclose(marker);
        } else {
            OH_LOG_ERROR(LOG_APP, "[Launch-Async] cannot create prefix init marker: %{public}s",
                         initMarker.c_str());
            return false;
        }
        auto* ws = WaylandServer::GetInstance();
        ws->SetDesktopRootRecognitionEnabled(false);
        // wineboot creates shell-owned helper windows while initializing a fresh
        // prefix.  Keep those helpers on the desktop path even when the smoke
        // suite itself uses managed windows; otherwise the first clean-prefix
        // session can leave Wayland/audio/graphics services half initialized.
        const char* desktopTag =
            (ws->IsDesktopMode() || p->automationMode) ? "__winehua_desktop__|" : "";
#ifdef __aarch64__
        std::string entryParams = p->homeDir + "|" + p->winehuaBin + "|" + desktopTag +
            "wineboot|--init|__env=WINEPREFIX=" + p->prefixDir;
#else
        std::string entryParams = p->homeDir + "|" + p->winehuaBin + "|" + desktopTag +
            "wine|wineboot|--init|__env=WINEPREFIX=" + p->prefixDir;
#endif
        // 注意: wineboot --init 只需要初始化 prefix, 不传完整环境变量以节省 entryParams 长度
        NativeChildProcess_Args childArgs = {};
        childArgs.entryParams = const_cast<char*>(entryParams.c_str());
        NativeChildProcess_Options options = {};
        options.isolationMode = NCP_ISOLATION_MODE_NORMAL;
        int32_t childPid = -1;
        auto ret = OH_Ability_StartNativeChildProcess(
            "libwine_child.so:Main", childArgs, options, &childPid);
        if (ret != NCP_NO_ERROR) {
            OH_LOG_ERROR(LOG_APP, "[Launch-Async] wineboot FAILED ret=%{public}d", (int)ret);
            if (gStateTsfn)
                napi_call_threadsafe_function(gStateTsfn, strdup("wineboot-failed"), napi_tsfn_blocking);
            return false;
        }
        OH_LOG_INFO(LOG_APP, "[Launch-Async] wineboot started, pid=%{public}d", childPid);
        /* 首次初始化 (wine.inf 的 PreInstall/DefaultInstall/Wow64Install + 可选 Mono)
         * 耗时与设备性能强相关, 模拟器上可超过 30s — 固定死线会把仍在正常初始化的
         * wineboot 误判为失败。改为进程活性驱动: wineboot 活着就继续等,
         * 大超时仅作挂死安全网; 真正的失败由进程退出后 prefix 不完整触发。 */
        char procPath[64];
        snprintf(procPath, sizeof(procPath), "/proc/%d", childPid);
        constexpr int kWinebootHangMs = 3 * 60 * 1000;
        int aliveMs = 0;
        while (IsProcessAliveNotZombie(childPid) && aliveMs < kWinebootHangMs) {
            usleep(500000);
            aliveMs += 500;
            if (aliveMs % 10000 == 0)
                OH_LOG_INFO(LOG_APP, "[Launch-Async] wineboot still initializing (%{public}d s)",
                            aliveMs / 1000);
        }
        if (aliveMs >= kWinebootHangMs) {
            OH_LOG_ERROR(LOG_APP, "[Launch-Async] wineboot hung for %{public}d s, abort",
                         kWinebootHangMs / 1000);
            if (gStateTsfn)
                napi_call_threadsafe_function(gStateTsfn, strdup("wineboot-failed"), napi_tsfn_blocking);
            return false;
        }
        /* wineboot 已退出: registry 仍在 wineserver flush 途中 (实测落盘延迟
         * 稳定 ~13s), 宽限窗口等文件就绪 — 文件到位即通过, 不会满等 */
        if (!WaitFor("wine prefix",
                     [&p]() { return IsWinePrefixInitialized(p->prefixDir); },
                     60000, 200)) {
            OH_LOG_ERROR(LOG_APP, "[Launch-Async] wineboot exited but prefix incomplete, abort");
            if (gStateTsfn)
                napi_call_threadsafe_function(gStateTsfn, strdup("wineboot-failed"), napi_tsfn_blocking);
            return false;
        }
        OH_LOG_INFO(LOG_APP, "[Launch-Async] wineboot completed (%{public}d s)", aliveMs / 1000);
        unlink(initMarker.c_str());
        ws->SetDesktopRootRecognitionEnabled(true);
        ws->PromotePendingDesktopRoot();
    } else {
        OH_LOG_INFO(LOG_APP, "[Launch-Async] prefix already initialized, skipping wineboot");
    }

    // -- explorer desktop shell (仅 desktop 模式) --
    PrepareDesktopSessionGraphicsEnv(*p);

    if (p->automationMode)
    {
        OH_LOG_INFO(LOG_APP, "[Launch-Async] automation session ready; Explorer intentionally skipped");
    }
    else if (WaylandServer::GetInstance()->IsDesktopMode())
    // -- explorer (Desktop 或 Pad 模式均启动) --
    {
        auto* ws = WaylandServer::GetInstance();
        ws->SetDesktopRootRecognitionEnabled(true);
        int dw = ws->outputW_ > 0 ? ws->outputW_ : 1280;
        int dh = ws->outputH_ > 0 ? ws->outputH_ : 720;
        OH_LOG_INFO(LOG_APP, "[Launch-Async] explorer desktop size: outputW=%{public}d outputH=%{public}d → %{public}dx%{public}d",
                    ws->outputW_, ws->outputH_, dw, dh);
        char desktopArg[128];
        snprintf(desktopArg, sizeof(desktopArg), "/desktop=shell,%dx%d", dw, dh);
#ifdef __aarch64__
        std::string exEntry = p->homeDir + "|" + p->winehuaBin + serializedEnv + "|__winehua_desktop__|explorer|" + desktopArg;
#else
        std::string exEntry = p->homeDir + "|" + p->winehuaBin + serializedEnv + "|__winehua_desktop__|wine|explorer|" + desktopArg;
#endif
        AppendDesktopD3dEntryEnv(exEntry, *p);
        NativeChildProcess_Args exArgs = {};
        exArgs.entryParams = const_cast<char*>(exEntry.c_str());
        exArgs.fdList.head = (audioBootstrapFd >= 0) ? &audioFdNode : nullptr;
        NativeChildProcess_Options exOpts = {};
        exOpts.isolationMode = NCP_ISOLATION_MODE_NORMAL;
        int32_t exPid = -1;
        auto exRet = OH_Ability_StartNativeChildProcess(
            "libwine_child.so:Main", exArgs, exOpts, &exPid);
        OH_LOG_INFO(LOG_APP, "[Launch-Async] explorer desktop pid=%{public}d ret=%{public}d",
                    exPid, (int)exRet);
        ws->PromotePendingDesktopRoot();
    }
    else
    {
        // 非桌面模式: 启动 explorer 文件管理器窗口
#ifdef __aarch64__
        std::string exEntry = p->homeDir + "|" + p->winehuaBin + serializedEnv + "|explorer";
#else
        std::string exEntry = p->homeDir + "|" + p->winehuaBin + serializedEnv + "|wine|explorer";
#endif
        AppendDesktopD3dEntryEnv(exEntry, *p);
        NativeChildProcess_Args exArgs = {};
        exArgs.entryParams = const_cast<char*>(exEntry.c_str());
        NativeChildProcess_Options exOpts = {};
        exOpts.isolationMode = NCP_ISOLATION_MODE_NORMAL;
        int32_t exPid = -1;
        auto exRet = OH_Ability_StartNativeChildProcess(
            "libwine_child.so:Main", exArgs, exOpts, &exPid);
        OH_LOG_INFO(LOG_APP, "[Launch-Async] explorer window pid=%{public}d ret=%{public}d",
                    exPid, (int)exRet);
    }
    return true;
}

void LaunchThreadFunc(LaunchParams* p) {
    OH_LOG_INFO(LOG_APP, "[Launch-Async] wineserver + wineboot + wine starting in background");
    OH_LOG_INFO(LOG_APP, "[Launch-Async] XKB_CONFIG_ROOT=%{public}s",
                (p->winehuaBin + "/../share/X11/xkb").c_str());

    auto& graphicsBroker = winehua::GraphicsBroker::GetInstance();
    graphicsBroker.SetWineRuntimeBinaryDir(p->winehuaBin);
    graphicsBroker.SetVulkanPresentMode(p->d3dBackend.rfind("dxvk_", 0) == 0);
    graphicsBroker.EnsureStarted(p->sockDir);

    int audioBootstrapFd = CreateAudioBootstrapFd(p->sockDir);
    // Resolve VirGL/backend state before serializing the environment so NCP
    // children inherit the active receiver rather than an early SHM snapshot.
    PrepareDesktopSessionGraphicsEnv(*p);
    p->envStrs = BuildWineEnv(p->sockDir, p->sockName, p->libPath, p->winehuaBin,
                               audioBootstrapFd, p->homeDir, p->prefixDir);
    AppendD3dBackendEnv(p->envStrs, p->d3dBackend, p->winehuaBin);
    const std::string serializedEnv = SerializeEnvToEntryParams(p->envStrs);

    mkdir(p->prefixDir.c_str(), 0755);

    if (gStateTsfn)
        napi_call_threadsafe_function(gStateTsfn, strdup("wineserver-starting"), napi_tsfn_blocking);

    bool ok = false;
    ok = LaunchPadMode(p, audioBootstrapFd, serializedEnv);

    if (ok && gStateTsfn)
        napi_call_threadsafe_function(gStateTsfn, strdup("wine-ready"), napi_tsfn_blocking);

    delete p;
}
