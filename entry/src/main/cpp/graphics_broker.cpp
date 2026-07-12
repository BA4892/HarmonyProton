#include "graphics_broker.h"

#include "wait_utils.h"
#include "wayland_server.h"

#ifdef __OHOS__
#include <AbilityKit/native_child_process.h>
#endif

#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
#include <fstream>
#include <signal.h>
#include <thread>

#undef LOG_TAG
#define LOG_TAG "WL_GFX"
#include <hilog/log.h>

namespace winehua {

namespace {

using PFNWinehuaVtestMain = int (*)(int argc, char** argv);

constexpr const char* VIRGL_SERVER_PROGRAM = "virgl_test_server";
constexpr const char* VIRGL_VTEST_LIBRARY = "libwinehua_vtest_server.so";
constexpr const char* GUEST_GFX_DIRNAME = "guest_gfx";
constexpr const char* GUEST_GFX_ENVFILE = "winehua-guest-gfx.env";

bool EnsureDir(const std::string& path)
{
    if (path.empty()) return false;
    return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

bool FileExists(const std::string& path)
{
    return !path.empty() && access(path.c_str(), F_OK) == 0;
}

bool DirExists(const std::string& path)
{
    struct stat st = {};

    if (path.empty()) return false;
    if (stat(path.c_str(), &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

bool DirHasSharedObjectWithPrefix(const std::string& dir, const std::string& prefix)
{
    DIR* handle = nullptr;
    struct dirent* entry = nullptr;

    if (dir.empty() || prefix.empty()) return false;
    handle = opendir(dir.c_str());
    if (!handle) return false;

    while ((entry = readdir(handle)))
    {
        std::string name = entry->d_name;
        if (name.rfind(prefix, 0) != 0) continue;
        if (name.find(".so") == std::string::npos) continue;
        closedir(handle);
        return true;
    }

    closedir(handle);
    return false;
}

std::string DirNameCopy(const std::string& path)
{
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

std::string CurrentSharedObjectDir()
{
    Dl_info info = {};
    if (dladdr(reinterpret_cast<void*>(&CurrentSharedObjectDir), &info) && info.dli_fname && info.dli_fname[0])
        return DirNameCopy(info.dli_fname);
    return "";
}

std::string ToLower(std::string value)
{
    for (char& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}

std::string TrimCopy(const std::string& value)
{
    size_t start = 0;
    size_t end = value.size();

    while (start < end && std::isspace(static_cast<unsigned char>(value[start]))) ++start;
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(start, end - start);
}

void ReplaceAll(std::string& value, const std::string& needle, const std::string& replacement)
{
    size_t pos = 0;

    if (needle.empty()) return;

    while ((pos = value.find(needle, pos)) != std::string::npos)
    {
        value.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
}

bool LoadGuestReceiverEnvFile(const std::string& receiverDir,
                              const std::string& envPath,
                              std::vector<std::string>& envLines,
                              std::string& mode)
{
    std::ifstream input(envPath);
    std::string line;

    if (!input.is_open()) return false;

    while (std::getline(input, line))
    {
        size_t equals;
        std::string key;
        std::string value;

        line = TrimCopy(line);
        if (line.empty() || line[0] == '#') continue;
        if (!line.compare(0, 7, "export ")) line = TrimCopy(line.substr(7));

        equals = line.find('=');
        if (equals == std::string::npos) continue;

        key = TrimCopy(line.substr(0, equals));
        value = TrimCopy(line.substr(equals + 1));
        if (key.empty()) continue;

        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\'')))
        {
            value = value.substr(1, value.size() - 2);
        }

        ReplaceAll(value, "$ORIGIN", receiverDir);
        if (key == "WINEHUA_GUEST_GFX_MODE") mode = value;

        if (key == "LD_LIBRARY_PATH" || key == "BOX64_LD_LIBRARY_PATH") continue;
        envLines.push_back(key + "=" + value);
    }

    return true;
}

std::string DescribeWaitStatus(int status)
{
    if (WIFEXITED(status))
    {
        return "exited code=" + std::to_string(WEXITSTATUS(status));
    }
    if (WIFSIGNALED(status))
    {
        return "signaled signal=" + std::to_string(WTERMSIG(status));
    }
    return "status=" + std::to_string(status);
}

void StartChildLogReader(int fd, pid_t pid, const char* tag)
{
    std::thread([fd, pid, tag]() {
        char buf[2048];
        std::string pending;

        while (true)
        {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n > 0)
            {
                pending.append(buf, static_cast<size_t>(n));
                while (true)
                {
                    size_t newline = pending.find('\n');
                    if (newline == std::string::npos) break;

                    std::string line = pending.substr(0, newline);
                    if (!line.empty())
                    {
                        OH_LOG_INFO(LOG_APP, "[%{public}s:%{public}d] %{public}s", tag, pid, line.c_str());
                    }
                    pending.erase(0, newline + 1);
                }
                continue;
            }

            if (n == 0) break;
            if (errno == EINTR) continue;

            OH_LOG_WARN(LOG_APP, "[%{public}s:%{public}d] read failed: %{public}s", tag, pid, strerror(errno));
            break;
        }

        if (!pending.empty())
        {
            OH_LOG_INFO(LOG_APP, "[%{public}s:%{public}d] %{public}s", tag, pid, pending.c_str());
        }
        close(fd);
    }).detach();
}

bool IsProcessRunningBySignal(pid_t pid)
{
    if (pid <= 0) return false;
    if (kill(pid, 0) == 0) return true;
    return errno == EPERM;
}

void TerminateTrackedProcess(pid_t pid, bool usesNcp)
{
    if (pid <= 0) return;

    kill(pid, SIGTERM);
    if (usesNcp)
    {
        WaitFor("virgl native child exit", [pid]() { return !IsProcessRunningBySignal(pid); }, 2000, 100);
        if (IsProcessRunningBySignal(pid)) kill(pid, SIGKILL);
        return;
    }

    for (int i = 0; i < 20; ++i)
    {
        int status = 0;
        pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid || (waited < 0 && errno == ECHILD)) return;
        usleep(100000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
}

} // namespace

GraphicsBroker& GraphicsBroker::GetInstance()
{
    static GraphicsBroker broker;
    return broker;
}

GraphicsBroker::GraphicsBroker()
{
    const char* requested = std::getenv("WINEHUA_GRAPHICS_BACKEND");
    GraphicsBackend backend;

    if (requested && ParseBackendName(requested, &backend)) {
        requestedBackend_ = backend;
    }
}

void GraphicsBroker::SetWineRuntimeBinaryDir(const std::string& wineBinDir)
{
    std::lock_guard<std::mutex> lock(mutex_);

    wineRuntimeBinDir_ = wineBinDir;
    virglServerProgramPath_.clear();
    virglVtestLibraryPath_.clear();
    if (!wineRuntimeBinDir_.empty())
    {
        std::string bundleDir = CurrentSharedObjectDir();
        std::string bundleVtestLibrary;

        virglServerProgramPath_ = wineRuntimeBinDir_ + "/" + VIRGL_SERVER_PROGRAM;
        if (!bundleDir.empty())
            bundleVtestLibrary = bundleDir + "/" + VIRGL_VTEST_LIBRARY;

        if (FileExists(bundleVtestLibrary))
            virglVtestLibraryPath_ = bundleVtestLibrary;

        OH_LOG_INFO(LOG_APP, "[GraphicsBroker] host helper=%{public}s server=%{public}s",
                    virglVtestLibraryPath_.empty() ? "(none)" : virglVtestLibraryPath_.c_str(),
                    virglServerProgramPath_.c_str());
    }
    RefreshGuestReceiverStateLocked();
}

bool GraphicsBroker::EnsureStarted(const std::string& runtimeDir)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!EnsureRuntimeLocked(runtimeDir)) {
        lastError_ = "failed to prepare graphics runtime directory";
        return false;
    }

    RefreshGuestReceiverStateLocked();
    started_ = true;
    if (requestedBackend_ == GraphicsBackend::Virgl) {
        RefreshVirglStateLocked();
        StartVirglSocketServerLocked();
    } else {
        lastError_.clear();
    }
    UpdateActiveBackendLocked();
    return true;
}

void GraphicsBroker::Stop()
{
    std::string socketPath;
    int serverPid = -1;
    bool serverUsesNcp = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        virglServerRunning_.store(false, std::memory_order_release);
        socketPath = virglSocketPath_;
        serverPid = virglServerPid_;
        serverUsesNcp = virglServerUsesNcp_;
        virglServerPid_ = -1;
        virglServerUsesNcp_ = false;
        virglSocketReady_ = false;
        activeBackend_ = GraphicsBackend::Shm;
        started_ = false;
        runtimeReady_ = false;
    }

    if (serverPid > 0)
    {
        TerminateTrackedProcess(serverPid, serverUsesNcp);
    }
    if (!socketPath.empty()) unlink(socketPath.c_str());
}

void GraphicsBroker::SetRequestedBackend(GraphicsBackend backend)
{
    std::lock_guard<std::mutex> lock(mutex_);

    requestedBackend_ = backend;
    loggedVirglFallback_ = false;
    if (started_ && requestedBackend_ == GraphicsBackend::Virgl) {
        RefreshVirglStateLocked();
        StartVirglSocketServerLocked();
    } else if (requestedBackend_ == GraphicsBackend::Shm) {
        lastError_.clear();
    }
    UpdateActiveBackendLocked();
}

GraphicsBackendState GraphicsBroker::GetState() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    GraphicsBackendState state;
    state.requested = requestedBackend_;
    state.active = activeBackend_;
    state.runtimeReady = runtimeReady_;
    state.guestReceiverPresent = guestReceiverPresent_;
    state.guestReceiverRuntimeDir = guestReceiverRuntimeDir_;
    state.guestReceiverMode = guestReceiverMode_;
    state.guestReceiverError = guestReceiverError_;
    state.virglSocketReady = virglSocketReady_;
    state.virglLibraryPresent = virglLibraryPresent_;
    state.zeroCopyFramePath = false;
    state.runtimeDir = runtimeDir_;
    state.virglSocketPath = virglSocketPath_;
    state.virglLibraryPath = virglLibraryPath_;
    state.frameTransportMode = "wl_shm+cpu_copy+gl_upload";
    state.lastError = lastError_;
    return state;
}

void GraphicsBroker::AppendWineEnv(std::vector<std::string>& env) const
{
    GraphicsBackendState state = GetState();
    std::vector<std::string> guestEnv;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        guestEnv = guestReceiverEnv_;
    }

    env.push_back("WINEHUA_GRAPHICS_BACKEND=" + std::string(BackendName(state.requested)));
    env.push_back("WINEHUA_GRAPHICS_ACTIVE=" + std::string(BackendName(state.active)));
    env.push_back(std::string("WINEHUA_SHM_FALLBACK=") + (state.active == GraphicsBackend::Shm ? "1" : "0"));
    env.push_back("WINEHUA_FRAME_ZERO_COPY=0");
    env.push_back("WINEHUA_FRAME_TRANSPORT=" + state.frameTransportMode);
    env.push_back(std::string("WINEHUA_GUEST_GFX_READY=") + (state.guestReceiverPresent ? "1" : "0"));
    env.push_back("WINEHUA_GUEST_GFX_MODE=" + (state.guestReceiverMode.empty() ? std::string("stock-egl")
                                                                                : state.guestReceiverMode));
    if (!state.guestReceiverRuntimeDir.empty()) env.push_back("WINEHUA_GUEST_GFX_DIR=" + state.guestReceiverRuntimeDir);
    env.push_back(std::string("WINEHUA_VIRGL_SOCKET_READY=") + (state.virglSocketReady ? "1" : "0"));
    env.push_back(std::string("WINEHUA_VIRGL_LIBRARY_READY=") + (state.virglLibraryPresent ? "1" : "0"));
    if (!state.virglSocketPath.empty()) env.push_back("WINEHUA_VIRGL_SOCKET=" + state.virglSocketPath);
    if (!state.virglLibraryPath.empty()) env.push_back("WINEHUA_VIRGLRENDERER_LIB=" + state.virglLibraryPath);
    if (!state.lastError.empty()) env.push_back("WINEHUA_GRAPHICS_NOTE=" + state.lastError);
    env.push_back(std::string("WINEHUA_VIRGL_READY=") +
                  ((state.active == GraphicsBackend::Virgl) ? "1" : "0"));
    if (state.active == GraphicsBackend::Virgl)
    {
        if (!state.guestReceiverRuntimeDir.empty())
        {
            std::string guestLibDir = state.guestReceiverRuntimeDir + "/lib";
            env.push_back("EGL_PLATFORM=wayland");
            if (FileExists(guestLibDir + "/libEGL.so"))
                env.push_back("WINEHUA_EGL_LIBRARY_PATH=" + guestLibDir + "/libEGL.so");
            env.push_back("BOX64_EMULATED_LIBS=libEGL.so:libEGL.so.1:libGLESv2.so:libGLESv2.so.2:"
                          "libGLESv1_CM.so:libGLESv1_CM.so.1:libGL.so:libGL.so.1:"
                          "libwayland-client.so:libwayland-client.so.0:libwayland-server.so:"
                          "libwayland-server.so.0:libwayland-egl.so:libwayland-egl.so.1:"
                          "libdrm.so:libdrm.so.2:libffi.so:libffi.so.8");
            if (DirExists(guestLibDir + "/dri")) env.push_back("LIBGL_DRIVERS_PATH=" + guestLibDir + "/dri");
            if (DirExists(guestLibDir + "/egl")) env.push_back("EGL_DRIVERS_PATH=" + guestLibDir + "/egl");
        }
        env.push_back("WINEHUA_WAYLAND_READBACK=1");
        env.push_back("WINEHUA_GL_STALL_DIAG=1");
        env.push_back("WINEHUA_DISPLAY_FPS_FILE=C:\\windows\\temp\\winehua_display_fps.txt");
        for (const std::string& extra : guestEnv) env.push_back(extra);
        if (!state.virglSocketPath.empty()) env.push_back("VTEST_SOCKET_NAME=" + state.virglSocketPath);
    }
}

bool GraphicsBroker::TakeFrameForToplevel(uint32_t rendererToplevelId,
                                          std::vector<uint8_t>& outPixels,
                                          int& w,
                                          int& h,
                                          uint32_t* outSourceToplevelId)
{
    WaylandServer* ws = WaylandServer::GetInstance();
    uint32_t sourceToplevelId = rendererToplevelId;

    if (ws->IsDesktopMode()) sourceToplevelId = ws->GetDesktopRootToplevelId();
    if (outSourceToplevelId) *outSourceToplevelId = sourceToplevelId;

    if (sourceToplevelId != 0) return ws->TakeToplevelFrame(sourceToplevelId, outPixels, w, h);
    return ws->TakeFrame(outPixels, w, h);
}

const char* GraphicsBroker::BackendName(GraphicsBackend backend)
{
    switch (backend) {
    case GraphicsBackend::Virgl:
        return "virgl";
    case GraphicsBackend::Shm:
    default:
        return "shm";
    }
}

bool GraphicsBroker::ParseBackendName(const std::string& name, GraphicsBackend* outBackend)
{
    if (!outBackend) return false;

    std::string lower = ToLower(name);
    if (lower == "shm") {
        *outBackend = GraphicsBackend::Shm;
        return true;
    }
    if (lower == "virgl") {
        *outBackend = GraphicsBackend::Virgl;
        return true;
    }
    return false;
}

bool GraphicsBroker::EnsureRuntimeLocked(const std::string& runtimeDir)
{
    if (!EnsureDir(runtimeDir)) return false;

    runtimeDir_ = runtimeDir + "/graphics";
    virglSocketPath_ = runtimeDir_ + "/virgl.sock";
    runtimeReady_ = EnsureDir(runtimeDir_);
    return runtimeReady_;
}

bool GraphicsBroker::IsVirglServerProcessAliveLocked()
{
#ifdef __OHOS__
    if (virglServerUsesNcp_)
    {
        if (IsProcessRunningBySignal(virglServerPid_)) return true;

        lastError_ = "virgl native child process is not running";
        virglServerPid_ = -1;
        virglServerUsesNcp_ = false;
        virglServerRunning_.store(false, std::memory_order_release);
        virglSocketReady_ = false;
        return false;
    }
#endif
    int status = 0;
    pid_t waited = 0;

    if (virglServerPid_ <= 0) return false;

    waited = waitpid(virglServerPid_, &status, WNOHANG);
    if (waited == 0) return true;

    if (waited == virglServerPid_)
    {
        std::string statusText = DescribeWaitStatus(status);
        lastError_ = "virgl_test_server terminated: " + statusText;
        OH_LOG_WARN(LOG_APP,
                    "[GraphicsBroker] virgl_test_server pid=%{public}d exited before guest connection (%{public}s)",
                    virglServerPid_, statusText.c_str());
    }
    virglServerPid_ = -1;
    virglServerUsesNcp_ = false;
    virglServerRunning_.store(false, std::memory_order_release);
    virglSocketReady_ = false;
    return false;
}

void GraphicsBroker::RefreshVirglStateLocked()
{
    bool loaded = false;
    virglLibraryPath_ = ProbeVirglLibraryLocked(&loaded);
    virglLibraryPresent_ = loaded;
    if (!virglLibraryPresent_) lastError_ = "virglrenderer library not found; using shm fallback";
}

void GraphicsBroker::RefreshGuestReceiverStateLocked()
{
    std::string receiverDir;
    std::string envPath;
    std::string libDir;
    std::vector<std::string> envLines;
    std::string mode;
    bool hasLibEGL = false;
    bool hasClientApi = false;
    bool hasDriverPayload = false;

    guestReceiverPresent_ = false;
    guestReceiverRuntimeDir_.clear();
    guestReceiverMode_.clear();
    guestReceiverError_.clear();
    guestReceiverEnv_.clear();

    if (wineRuntimeBinDir_.empty()) return;

    receiverDir = wineRuntimeBinDir_ + "/" + GUEST_GFX_DIRNAME;
    envPath = receiverDir + "/" + GUEST_GFX_ENVFILE;
    if (!FileExists(envPath))
    {
        guestReceiverError_ = "guest receiver env missing: " + envPath;
        return;
    }
    if (!LoadGuestReceiverEnvFile(receiverDir, envPath, envLines, mode))
    {
        guestReceiverError_ = "failed to parse guest receiver env: " + envPath;
        return;
    }

    guestReceiverRuntimeDir_ = receiverDir;
    guestReceiverMode_ = mode.empty() ? "external-bundle" : mode;

    libDir = receiverDir + "/lib";
    if (!DirExists(libDir))
    {
        guestReceiverError_ = "guest receiver lib dir missing: " + libDir;
        return;
    }

    hasLibEGL = FileExists(libDir + "/libEGL.so") || FileExists(libDir + "/libEGL.so.1");
    if (!hasLibEGL)
    {
        guestReceiverError_ = "guest receiver is missing libEGL.so* in " + libDir;
        return;
    }

    hasClientApi = FileExists(libDir + "/libGL.so") || FileExists(libDir + "/libGL.so.1") ||
                   FileExists(libDir + "/libOpenGL.so") || FileExists(libDir + "/libOpenGL.so.0") ||
                   FileExists(libDir + "/libGLESv2.so") || FileExists(libDir + "/libGLESv2.so.2") ||
                   FileExists(libDir + "/libGLESv1_CM.so") || FileExists(libDir + "/libGLESv1_CM.so.1");
    if (!hasClientApi)
    {
        guestReceiverError_ = "guest receiver is missing libGL.so* or libGLESv2.so* in " + libDir;
        return;
    }

    hasDriverPayload = DirExists(libDir + "/dri") || DirExists(libDir + "/egl") || DirExists(libDir + "/gallium") ||
                       FileExists(libDir + "/libgallium_dri.so") ||
                       DirHasSharedObjectWithPrefix(libDir, "libgallium-");
    if (!hasDriverPayload)
    {
        guestReceiverError_ = "guest receiver is missing Mesa driver payloads (dri/egl/gallium/libgallium-*.so) in " + libDir;
        return;
    }

    if (mode.find("virpipe") != std::string::npos && !FileExists(libDir + "/dri/virtio_gpu_dri.so"))
    {
        guestReceiverError_ = "guest receiver is missing lib/dri/virtio_gpu_dri.so in " + libDir;
        return;
    }

    guestReceiverPresent_ = true;
    guestReceiverEnv_ = std::move(envLines);

    OH_LOG_INFO(LOG_APP,
                "[GraphicsBroker] guest 3D receiver bundle detected mode=%{public}s dir=%{public}s",
                guestReceiverMode_.c_str(),
                guestReceiverRuntimeDir_.c_str());
}

void GraphicsBroker::StartVirglSocketServerLocked()
{
    std::string serverPath;
    std::string serverDir;
    std::string ldLibraryPath;
    int logPipe[2] = {-1, -1};
    bool logPipeOk = false;
    bool usingDedicatedHostRuntime = false;
    bool usingCallableVtest = false;
    void* vtestHandle = nullptr;
    PFNWinehuaVtestMain vtestMain = nullptr;
    pid_t pid = -1;

    if (!runtimeReady_ || virglSocketPath_.empty()) return;
    if (virglServerRunning_.load(std::memory_order_acquire) && IsVirglServerProcessAliveLocked()) return;
    if (wineRuntimeBinDir_.empty())
    {
        lastError_ = "wine runtime bin dir is not configured; using shm fallback";
        return;
    }

#ifdef __OHOS__
    if (virglVtestLibraryPath_.empty() || !FileExists(virglVtestLibraryPath_))
    {
        lastError_ = "ARM64 virgl vtest helper is missing from the bundle";
        return;
    }

    serverDir = DirNameCopy(virglVtestLibraryPath_);
    ldLibraryPath = serverDir;
    unlink(virglSocketPath_.c_str());
    {
        const char* requestedSyncMode = getenv("WINEHUA_VIRGL_SYNC_MODE");
        std::string syncMode = requestedSyncMode ? requestedSyncMode : "egl-main";
        if (syncMode != "egl-thread" && syncMode != "egl-main" && syncMode != "native-fd")
        {
            OH_LOG_WARN(LOG_APP, "[GraphicsBroker] invalid sync mode %{public}s; using egl-thread",
                        syncMode.c_str());
            syncMode = "egl-thread";
        }
        std::string virglLogPath = "/data/storage/el2/base/cache/winehua_virgl_host.log";
        std::string entryParams = virglVtestLibraryPath_ + "|" + virglSocketPath_ +
                                  "|__env=LD_LIBRARY_PATH=" + ldLibraryPath +
                                  "|__env=VTEST_USE_GLES=1" +
                                  "|__env=VTEST_USE_EGL_SURFACELESS=1" +
                                  "|__env=VTEST_SYNC_GL_FINISH=1" +
                                  "|__env=WINEHUA_VIRGL_SYNC_MODE=" + syncMode +
                                  "|__env=WINEHUA_VIRGL_LOG_PATH=" + virglLogPath +
                                  "|__env=EGL_PLATFORM=surfaceless";
        if (syncMode == "egl-thread")
            entryParams += "|__env=VIRGL_DISABLE_NATIVE_FENCE_FD=1";
        NativeChildProcess_Args childArgs = {};
        NativeChildProcess_Options options = {};
        int32_t childPid = -1;

        childArgs.entryParams = const_cast<char*>(entryParams.c_str());
        options.isolationMode = NCP_ISOLATION_MODE_NORMAL;
        int32_t ret = OH_Ability_StartNativeChildProcess(
            "libvirgl_child.so:Main", childArgs, options, &childPid);
        OH_LOG_INFO(LOG_APP,
                    "[GraphicsBroker] NCP virgl_child ret=%{public}d pid=%{public}d helper=%{public}s "
                    "socket=%{public}s hostLib=%{public}s sync=%{public}s log=%{public}s",
                    ret, childPid, virglVtestLibraryPath_.c_str(), virglSocketPath_.c_str(),
                    ldLibraryPath.c_str(), syncMode.c_str(), virglLogPath.c_str());
        if (ret != NCP_NO_ERROR || childPid <= 0)
        {
            lastError_ = "failed to start virgl native child process ret=" + std::to_string(ret);
            virglServerPid_ = -1;
            virglServerUsesNcp_ = false;
            virglServerRunning_.store(false, std::memory_order_release);
            virglSocketReady_ = false;
            return;
        }
        pid = static_cast<pid_t>(childPid);
        virglServerUsesNcp_ = true;
        OH_LOG_INFO(LOG_APP, "[GraphicsBroker] virgl NCP child launched, waiting for socket...");
    }
#else
    serverPath = virglServerProgramPath_.empty() ? (wineRuntimeBinDir_ + "/virgl_test_server") : virglServerProgramPath_;
    if (!virglVtestLibraryPath_.empty())
    {
        vtestHandle = dlopen(virglVtestLibraryPath_.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!vtestHandle)
        {
            OH_LOG_WARN(LOG_APP, "[GraphicsBroker] dlopen %{public}s failed: %{public}s",
                        virglVtestLibraryPath_.c_str(), dlerror());
        }
        else
        {
            vtestMain = reinterpret_cast<PFNWinehuaVtestMain>(dlsym(vtestHandle, "winehua_vtest_main"));
            if (!vtestMain)
            {
                OH_LOG_WARN(LOG_APP, "[GraphicsBroker] winehua_vtest_main missing in %{public}s: %{public}s",
                            virglVtestLibraryPath_.c_str(), dlerror());
            }
            else
            {
                usingCallableVtest = true;
            }
        }
    }
    if (!usingCallableVtest && vtestHandle)
    {
        dlclose(vtestHandle);
        vtestHandle = nullptr;
    }

    if (!usingCallableVtest && access(serverPath.c_str(), X_OK) != 0)
    {
        lastError_ = "virgl vtest helper is unavailable and virgl_test_server is missing from runtime: " + serverPath;
        OH_LOG_WARN(LOG_APP, "[GraphicsBroker] %{public}s errno=%{public}d (%{public}s)",
                    lastError_.c_str(), errno, strerror(errno));
        return;
    }

    serverDir = usingCallableVtest ? DirNameCopy(virglVtestLibraryPath_) : DirNameCopy(serverPath);
    usingDedicatedHostRuntime = (serverDir != wineRuntimeBinDir_);
    if (usingDedicatedHostRuntime)
    {
        ldLibraryPath = serverDir;
    }
    else
    {
        ldLibraryPath = wineRuntimeBinDir_ + ":" + wineRuntimeBinDir_ + "/x86_64-unix:" + wineRuntimeBinDir_ + "/../lib/x86_64";
    }
    OH_LOG_INFO(LOG_APP,
                "[GraphicsBroker] virgl_test_server runtime: callable=%{public}d dedicatedHostRuntime=%{public}d serverDir=%{public}s libPath=%{public}s",
                usingCallableVtest ? 1 : 0,
                usingDedicatedHostRuntime ? 1 : 0,
                serverDir.c_str(),
                ldLibraryPath.c_str());
    logPipeOk = (pipe(logPipe) == 0);
    if (!logPipeOk)
    {
        OH_LOG_WARN(LOG_APP, "[GraphicsBroker] failed to create virgl_test_server log pipe: %{public}s",
                    strerror(errno));
    }

    unlink(virglSocketPath_.c_str());
    pid = fork();
    if (pid < 0)
    {
        if (logPipeOk)
        {
            close(logPipe[0]);
            close(logPipe[1]);
        }
        if (vtestHandle) dlclose(vtestHandle);
        lastError_ = std::string("failed to fork virgl_test_server: ") + strerror(errno);
        virglServerPid_ = -1;
        virglServerRunning_.store(false, std::memory_order_release);
        virglSocketReady_ = false;
        return;
    }

    if (pid == 0)
    {
        if (logPipeOk)
        {
            close(logPipe[0]);
            dup2(logPipe[1], STDOUT_FILENO);
            dup2(logPipe[1], STDERR_FILENO);
            if (logPipe[1] > 2) close(logPipe[1]);
        }
        setenv("LD_LIBRARY_PATH", ldLibraryPath.c_str(), 1);
        setenv("VTEST_USE_GLES", "1", 1);
        setenv("VTEST_USE_EGL_SURFACELESS", "1", 1);
        setenv("EGL_PLATFORM", "surfaceless", 1);
        unsetenv("GALLIUM_DRIVER");
        unsetenv("MESA_LOADER_DRIVER_OVERRIDE");
        unsetenv("LIBGL_ALWAYS_SOFTWARE");
        chdir(serverDir.c_str());
        if (usingCallableVtest && vtestMain)
        {
            char* args[] = {
                const_cast<char*>("virgl_test_server"),
                const_cast<char*>("--no-fork"),
                const_cast<char*>("--use-egl-surfaceless"),
                const_cast<char*>("--use-gles"),
                const_cast<char*>("--socket-path"),
                const_cast<char*>(virglSocketPath_.c_str()),
                nullptr,
            };
            int rc = vtestMain(6, args);
            dprintf(STDERR_FILENO, "virgl_test_server callable exited: %d\n", rc);
            _exit(rc == 0 ? 0 : 127);
        }
        execl(serverPath.c_str(),
              "virgl_test_server",
              "--no-fork",
              "--use-egl-surfaceless",
              "--use-gles",
              "--socket-path",
              virglSocketPath_.c_str(),
              nullptr);
        dprintf(STDERR_FILENO, "virgl_test_server exec failed: %s\n", strerror(errno));
        _exit(127);
    }

    if (logPipeOk)
    {
        close(logPipe[1]);
        StartChildLogReader(logPipe[0], pid, "virgl-test");
    }
    if (vtestHandle) dlclose(vtestHandle);
#endif

    virglServerPid_ = pid;
    virglServerRunning_.store(true, std::memory_order_release);
    virglSocketReady_ = false;

    OH_LOG_INFO(LOG_APP, "[GraphicsBroker] waiting for virgl socket at %{public}s pid=%{public}d",
                virglSocketPath_.c_str(), virglServerPid_);

    if (WaitFor("virgl_test_server socket",
                [this]() { return FileExists(virglSocketPath_) || !IsVirglServerProcessAliveLocked(); },
                4000, 100) &&
        FileExists(virglSocketPath_) &&
        IsVirglServerProcessAliveLocked())
    {
        virglSocketReady_ = true;
        lastError_ = "VirGL vtest server is up; waiting for guest-side 3D receiver";
        OH_LOG_INFO(LOG_APP,
                    "[GraphicsBroker] virgl_test_server pid=%{public}d listening at %{public}s",
                    virglServerPid_, virglSocketPath_.c_str());
        return;
    }

    OH_LOG_ERROR(LOG_APP,
                 "[GraphicsBroker] virgl socket wait FAILED: socket_exists=%{public}d process_alive=%{public}d",
                 FileExists(virglSocketPath_) ? 1 : 0,
                 IsVirglServerProcessAliveLocked() ? 1 : 0);

    if (virglServerPid_ > 0)
    {
        TerminateTrackedProcess(virglServerPid_, virglServerUsesNcp_);
    }
    virglServerPid_ = -1;
    virglServerUsesNcp_ = false;
    virglServerRunning_.store(false, std::memory_order_release);
    virglSocketReady_ = false;
    lastError_ = "timed out waiting for virgl_test_server socket";
}

void GraphicsBroker::UpdateActiveBackendLocked()
{
    if (requestedBackend_ == GraphicsBackend::Shm) {
        activeBackend_ = GraphicsBackend::Shm;
        loggedVirglFallback_ = false;
        return;
    }

    if (runtimeReady_ && virglLibraryPresent_ && virglSocketReady_ && guestReceiverPresent_)
    {
        activeBackend_ = GraphicsBackend::Virgl;
        loggedVirglFallback_ = false;
        lastError_.clear();
        return;
    }

    activeBackend_ = GraphicsBackend::Shm;
    if (!runtimeReady_) {
        lastError_ = "graphics runtime is not ready; using shm fallback";
    } else if (!virglLibraryPresent_) {
        lastError_ = "virglrenderer library not found; using shm fallback";
    } else if (!virglSocketReady_) {
        lastError_ = "virgl socket is not ready yet; using shm fallback";
    } else if (!guestReceiverPresent_) {
        if (!guestReceiverError_.empty()) {
            lastError_ = "virgl host is ready, but guest receiver bundle is incomplete: " + guestReceiverError_ +
                         "; Windows OpenGL/DX still uses stock wayland/EGL; using shm fallback";
        } else {
            lastError_ = "virgl host is ready, but no guest 3D receiver bundle was staged "
                         "(guest_gfx/winehua-guest-gfx.env missing); Windows OpenGL/DX still uses stock wayland/EGL; using shm fallback";
        }
    } else {
        lastError_ = "VirGL runtime prerequisites are not satisfied yet; using shm fallback";
    }

    if (!loggedVirglFallback_) {
        OH_LOG_WARN(LOG_APP, "[GraphicsBroker] requested backend=%{public}s active=%{public}s reason=%{public}s",
                    BackendName(requestedBackend_), BackendName(activeBackend_), lastError_.c_str());
        loggedVirglFallback_ = true;
    }
}

std::string GraphicsBroker::ProbeVirglLibraryLocked(bool* outLoaded) const
{
    const char* envLib = std::getenv("WINEHUA_VIRGLRENDERER_LIB");
    const char* candidates[] = {
        envLib && envLib[0] ? envLib : nullptr,
        "libvirglrenderer.so",
        "libvirglrenderer.so.1",
    };

    if (outLoaded) *outLoaded = false;

    for (const char* candidate : candidates) {
        if (!candidate || !candidate[0]) continue;

        void* handle = dlopen(candidate, RTLD_NOW | RTLD_LOCAL);
        if (!handle) continue;

        void* symbol = dlsym(handle, "virgl_renderer_init");
        dlclose(handle);
        if (!symbol) continue;

        if (outLoaded) *outLoaded = true;
        return candidate;
    }

    return "";
}

} // namespace winehua
