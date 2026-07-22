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
#define LOG_TAG "WL_NAPI"
#include <hilog/log.h>

#include "broker.h"
#include "wait_utils.h"

#include <AbilityKit/native_child_process.h>

// NCP 子进程中 environ 为空, 从 envp 重建供 wine 使用
static void rebuild_environ(char* const* envp) {
    extern char** environ;
    environ = (char**)envp;
}

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

static void AppendStableDesktopDxvkEnv(std::vector<std::string>& env,
                                       const LaunchParams& params)
{
    if (params.d3dBackend.rfind("dxvk_", 0) != 0) return;

    /* Explorer-launched programs inherit the desktop process environment and
     * bypass Index.d3dLaunchEnvironment(). Keep the product-correct settings
     * here without changing the explicit A/B profiles used by runWineProgram. */
    env.push_back("DXVK_LOG_LEVEL=info");
    env.push_back("DXVK_LOG_PATH=C:\\windows\\temp");
    env.push_back("BOX64_DYNAREC_WEAKBARRIER=0");
    env.push_back("WINEHUA_PERF_PROFILE=shadow-precise-strong-ring");
    env.push_back("DXVK_WINEHUA_PRECISE_SHADOW=1");
    env.push_back("VN_WINEHUA_STRONG_RING_BARRIER=1");
}

static void PrepareDesktopSessionGraphicsEnv(const LaunchParams& params)
{
    OH_LOG_INFO(LOG_APP, "[Launch-Async] preparing GL env for desktop child processes");
    auto& gb = winehua::GraphicsBroker::GetInstance();
    gb.SetWineRuntimeBinaryDir(params.winehuaBin);
    gb.SetRequestedBackend(winehua::GraphicsBackend::Virgl);
    gb.SetVulkanPresentMode(params.d3dBackend.rfind("dxvk_", 0) == 0);
    gb.EnsureStarted(params.sockDir);

    winehua::GraphicsBackendState state = gb.GetState();
    if (state.active != winehua::GraphicsBackend::Virgl) {
        OH_LOG_ERROR(LOG_APP,
                     "[Launch-Async] desktop GL env unavailable: requested=%{public}s active=%{public}s error=%{public}s",
                     winehua::GraphicsBroker::BackendName(state.requested),
                     winehua::GraphicsBroker::BackendName(state.active),
                     state.lastError.c_str());
        return;
    }

    std::vector<std::string> env;
    gb.AppendWineEnv(env);
    AppendD3dBackendEnv(env, params.d3dBackend, params.winehuaBin);
    AppendStableDesktopDxvkEnv(env, params);
    SetBrokerSessionEnv(std::move(env));
    LogGraphicsBackendStateForLaunch("DesktopSession");
}

static void AppendDesktopD3dEntryEnv(std::string& entryParams, const LaunchParams& params)
{
    std::vector<std::string> env;
    AppendD3dBackendEnv(env, params.d3dBackend, params.winehuaBin);
    AppendStableDesktopDxvkEnv(env, params);
    AppendMissingEntryParamsEnvOverrides(entryParams, env);
}

static bool LaunchPadMode(LaunchParams* p, int audioBootstrapFd) {
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
    ClearBrokerSessionEnv();
    StartBrokerServer();
    setenv("PROCESSBROKER", WINE_BROKER_SOCKET, 1);

    // -- wineboot --init --
    bool prefixReady = IsWinePrefixInitialized(p->prefixDir);

    if (!prefixReady) {
        OH_LOG_INFO(LOG_APP, "[Launch-Async] prefix not initialized, preparing WoW64 and running wineboot --init...");
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
        if (!WaitFor("wine prefix", [p]() { return IsWinePrefixInitialized(p->prefixDir); }, 60000, 200)) {
            OH_LOG_ERROR(LOG_APP, "[Launch-Async] wineboot drive_c timeout, abort");
            if (gStateTsfn)
                napi_call_threadsafe_function(gStateTsfn, strdup("wineboot-failed"), napi_tsfn_blocking);
            return false;
        }
        char procPath[64];
        snprintf(procPath, sizeof(procPath), "/proc/%d", childPid);
        for (int i = 0; i < 120; i++) {
            if (access(procPath, F_OK) != 0) break;
            usleep(500000);
        }
        OH_LOG_INFO(LOG_APP, "[Launch-Async] wineboot completed");
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
        std::string exEntry = p->homeDir + "|" + p->winehuaBin + "|__winehua_desktop__|explorer|" + desktopArg;
#else
        std::string exEntry = p->homeDir + "|" + p->winehuaBin + "|__winehua_desktop__|wine|explorer|" + desktopArg;
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
        std::string exEntry = p->homeDir + "|" + p->winehuaBin + "|explorer";
#else
        std::string exEntry = p->homeDir + "|" + p->winehuaBin + "|wine|explorer";
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
    p->envStrs = BuildWineEnv(p->sockDir, p->sockName, p->libPath, p->winehuaBin,
                               audioBootstrapFd, p->homeDir, p->prefixDir);
    AppendD3dBackendEnv(p->envStrs, p->d3dBackend, p->winehuaBin);
    for (auto& s : p->envStrs) p->envp.push_back((char*)s.c_str());
    p->envp.push_back(nullptr);

    mkdir(p->prefixDir.c_str(), 0755);

    if (gStateTsfn)
        napi_call_threadsafe_function(gStateTsfn, strdup("wineserver-starting"), napi_tsfn_blocking);

    bool ok = false;
    ok = LaunchPadMode(p, audioBootstrapFd);

    if (ok && gStateTsfn)
        napi_call_threadsafe_function(gStateTsfn, strdup("wine-ready"), napi_tsfn_blocking);

    delete p;
}
