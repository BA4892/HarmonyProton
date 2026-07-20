#include "graphics_broker.h"

#include "wait_utils.h"
#include "wayland_server.h"

#include <AbilityKit/native_child_process.h>
#include <IPCKit/ipc_kit.h>
#include <native_window/external_window.h>

#include "virgl_ipc_protocol.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
#include <fstream>
#include <fcntl.h>
#include <signal.h>
#include <thread>

#undef LOG_TAG
#define LOG_TAG "WL_GFX"
#include <hilog/log.h>

namespace winehua {

namespace {


constexpr const char* VIRGL_SERVER_PROGRAM = "virgl_test_server";
constexpr const char* VIRGL_VTEST_LIBRARY = "libwinehua_vtest_server.so";
constexpr const char* GUEST_GFX_DIRNAME = "guest_gfx";
constexpr const char* GUEST_GFX_ENVFILE = "winehua-guest-gfx.env";
constexpr const char* ZERO_COPY_READY_DIR = "/data/storage/el2/base/cache";
constexpr const char* ZERO_COPY_READY_PREFIX = "winehua_zc_surface_";

std::string ZeroCopyReadyPath(uint64_t surfaceKey)
{
    return std::string(ZERO_COPY_READY_DIR) + "/" + ZERO_COPY_READY_PREFIX +
           std::to_string(surfaceKey) + ".ready";
}

void RemoveStaleZeroCopyMarkers()
{
    DIR* dir = opendir(ZERO_COPY_READY_DIR);
    if (!dir) return;
    while (dirent* entry = readdir(dir))
    {
        const std::string name = entry->d_name;
        if (name.rfind(ZERO_COPY_READY_PREFIX, 0) != 0 ||
            name.size() < 6 || name.compare(name.size() - 6, 6, ".ready") != 0)
            continue;
        unlink((std::string(ZERO_COPY_READY_DIR) + "/" + name).c_str());
    }
    closedir(dir);
}

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

void GraphicsBroker::OnVirglIpcProcessStarted(int errorCode, OHIPCRemoteProxy* remoteProxy)
{
    GraphicsBroker& broker = GetInstance();
    std::unique_lock<std::mutex> lock(broker.virglIpcMutex_);

    broker.virglIpcError_ = errorCode;
    if (!broker.virglIpcAcceptCallback_ || errorCode != NCP_NO_ERROR || !remoteProxy)
    {
        if (remoteProxy) OH_IPCRemoteProxy_Destroy(remoteProxy);
        broker.virglIpcConfigured_ = false;
        broker.virglIpcCallbackComplete_ = true;
        lock.unlock();
        broker.virglIpcCondition_.notify_all();
        return;
    }

    if (broker.virglRemoteProxy_) OH_IPCRemoteProxy_Destroy(broker.virglRemoteProxy_);
    broker.virglRemoteProxy_ = remoteProxy;
    broker.virglIpcConfigured_ = broker.SendVirglConfigureLocked();
    broker.virglIpcCallbackComplete_ = true;

    OH_LOG_INFO(LOG_APP,
                "[VIRGL-ZC][MAIN] child IPC callback err=%{public}d configured=%{public}s",
                errorCode, broker.virglIpcConfigured_ ? "PASS" : "FAIL");
    lock.unlock();
    broker.virglIpcCondition_.notify_all();
}

bool GraphicsBroker::SendVirglConfigureLocked()
{
    if (!virglRemoteProxy_) return false;

    OHIPCParcel* request = OH_IPCParcel_Create();
    OHIPCParcel* reply = OH_IPCParcel_Create();
    int32_t writeResult = request
        ? OH_IPCParcel_WriteInt32(request, virgl_ipc::kProtocolVersion)
        : OH_IPC_MEM_ALLOCATOR_ERROR;
    if (writeResult == OH_IPC_SUCCESS)
        writeResult = OH_IPCParcel_WriteString(request, virglIpcHelperPath_.c_str());
    if (writeResult == OH_IPC_SUCCESS)
        writeResult = OH_IPCParcel_WriteString(request, virglIpcSocketPath_.c_str());
    if (writeResult == OH_IPC_SUCCESS)
        writeResult = OH_IPCParcel_WriteString(request, virglIpcLibraryPath_.c_str());
    if (writeResult == OH_IPC_SUCCESS)
        writeResult = OH_IPCParcel_WriteString(request, virglIpcSyncMode_.c_str());
    if (writeResult == OH_IPC_SUCCESS)
        writeResult = OH_IPCParcel_WriteString(request, virglIpcLogPath_.c_str());
    if (writeResult == OH_IPC_SUCCESS)
        writeResult = OH_IPCParcel_WriteString(request, virglIpcShadowMode_.c_str());
    if (writeResult == OH_IPC_SUCCESS)
        writeResult = OH_IPCParcel_WriteString(request, virglIpcShadowTrace_.c_str());

    int32_t childResult = -1;
    int32_t sendResult = writeResult;
    if (writeResult == OH_IPC_SUCCESS && reply)
    {
        OH_IPC_MessageOption option = {OH_IPC_REQUEST_MODE_SYNC, 0, nullptr};
        sendResult = OH_IPCRemoteProxy_SendRequest(
            virglRemoteProxy_, virgl_ipc::kConfigureRequest,
            request, reply, &option);
        if (sendResult == OH_IPC_SUCCESS)
            sendResult = OH_IPCParcel_ReadInt32(reply, &childResult);
    }

    if (reply) OH_IPCParcel_Destroy(reply);
    if (request) OH_IPCParcel_Destroy(request);
    return sendResult == OH_IPC_SUCCESS && childResult == 0;
}

bool GraphicsBroker::SendVirglTargetLocked(uint64_t surfaceKey,
                                           OHNativeWindow* producerWindow,
                                           uint64_t framePeriodNs,
                                           uint32_t flags)
{
    if (!virglRemoteProxy_ || !virglIpcConfigured_ || !producerWindow || !surfaceKey)
        return false;

    OHIPCParcel* request = OH_IPCParcel_Create();
    OHIPCParcel* reply = OH_IPCParcel_Create();
    int32_t writeResult = request
        ? OH_IPCParcel_WriteInt32(request, virgl_ipc::kProtocolVersion)
        : OH_IPC_MEM_ALLOCATOR_ERROR;
    if (writeResult == OH_IPC_SUCCESS)
        writeResult = OH_IPCParcel_WriteInt64(request, static_cast<int64_t>(surfaceKey));
    if (writeResult == OH_IPC_SUCCESS)
        writeResult = OH_IPCParcel_WriteInt64(request, static_cast<int64_t>(framePeriodNs));
    if (writeResult == OH_IPC_SUCCESS)
        writeResult = OH_IPCParcel_WriteInt32(request, static_cast<int32_t>(flags));
    if (writeResult == OH_IPC_SUCCESS)
        writeResult = OH_NativeWindow_WriteToParcel(producerWindow, request);

    int32_t childResult = -1;
    int32_t sendResult = writeResult;
    if (writeResult == OH_IPC_SUCCESS && reply)
    {
        OH_IPC_MessageOption option = {OH_IPC_REQUEST_MODE_SYNC, 0, nullptr};
        sendResult = OH_IPCRemoteProxy_SendRequest(
            virglRemoteProxy_, virgl_ipc::kAttachSurfaceRequest,
            request, reply, &option);
        if (sendResult == OH_IPC_SUCCESS)
            sendResult = OH_IPCParcel_ReadInt32(reply, &childResult);
    }

    if (reply) OH_IPCParcel_Destroy(reply);
    if (request) OH_IPCParcel_Destroy(request);
    OH_LOG_INFO(LOG_APP,
                "[VIRGL-ZC][MAIN] attach surface_key=%{public}llu period_ns=%{public}llu "
                "write=%{public}d send=%{public}d child=%{public}d",
                static_cast<unsigned long long>(surfaceKey),
                static_cast<unsigned long long>(framePeriodNs),
                writeResult, sendResult, childResult);
    return sendResult == OH_IPC_SUCCESS && childResult == 0;
}

bool GraphicsBroker::SendVirglFramePeriodLocked(uint64_t surfaceKey,
                                                uint64_t framePeriodNs)
{
    if (!virglRemoteProxy_ || !virglIpcConfigured_ || !surfaceKey || !framePeriodNs)
        return false;

    OHIPCParcel* request = OH_IPCParcel_Create();
    OHIPCParcel* reply = OH_IPCParcel_Create();
    int32_t writeResult = request
        ? OH_IPCParcel_WriteInt32(request, virgl_ipc::kProtocolVersion)
        : OH_IPC_MEM_ALLOCATOR_ERROR;
    if (writeResult == OH_IPC_SUCCESS)
        writeResult = OH_IPCParcel_WriteInt64(request, static_cast<int64_t>(surfaceKey));
    if (writeResult == OH_IPC_SUCCESS)
        writeResult = OH_IPCParcel_WriteInt64(request, static_cast<int64_t>(framePeriodNs));

    int32_t childResult = -1;
    int32_t sendResult = writeResult;
    if (writeResult == OH_IPC_SUCCESS && reply)
    {
        OH_IPC_MessageOption option = {OH_IPC_REQUEST_MODE_SYNC, 0, nullptr};
        sendResult = OH_IPCRemoteProxy_SendRequest(
            virglRemoteProxy_, virgl_ipc::kSetFramePeriodRequest,
            request, reply, &option);
        if (sendResult == OH_IPC_SUCCESS)
            sendResult = OH_IPCParcel_ReadInt32(reply, &childResult);
    }

    if (reply) OH_IPCParcel_Destroy(reply);
    if (request) OH_IPCParcel_Destroy(request);
    return sendResult == OH_IPC_SUCCESS && childResult == 0;
}

bool GraphicsBroker::SendVirglDetachLocked(uint64_t surfaceKey)
{
    if (!virglRemoteProxy_ || !virglIpcConfigured_) return false;

    OHIPCParcel* request = OH_IPCParcel_Create();
    OHIPCParcel* reply = OH_IPCParcel_Create();
    int32_t writeResult = request
        ? OH_IPCParcel_WriteInt32(request, virgl_ipc::kProtocolVersion)
        : OH_IPC_MEM_ALLOCATOR_ERROR;
    if (writeResult == OH_IPC_SUCCESS)
        writeResult = OH_IPCParcel_WriteInt64(request, static_cast<int64_t>(surfaceKey));

    int32_t childResult = -1;
    int32_t sendResult = writeResult;
    if (writeResult == OH_IPC_SUCCESS && reply)
    {
        OH_IPC_MessageOption option = {OH_IPC_REQUEST_MODE_SYNC, 0, nullptr};
        sendResult = OH_IPCRemoteProxy_SendRequest(
            virglRemoteProxy_, virgl_ipc::kDetachSurfaceRequest,
            request, reply, &option);
        if (sendResult == OH_IPC_SUCCESS)
            sendResult = OH_IPCParcel_ReadInt32(reply, &childResult);
    }

    if (reply) OH_IPCParcel_Destroy(reply);
    if (request) OH_IPCParcel_Destroy(request);
    return sendResult == OH_IPC_SUCCESS && childResult == 0;
}

bool GraphicsBroker::AttachZeroCopyTarget(uint64_t surfaceKey,
                                          OHNativeWindow* producerWindow,
                                          uint64_t framePeriodNs)
{
    if (!surfaceKey || !producerWindow || GetState().active != GraphicsBackend::Virgl)
        return false;

    std::lock_guard<std::mutex> lock(virglIpcMutex_);
    if (zeroCopyAttachedSurfaces_.count(surfaceKey)) return true;
    const uint32_t flags = vulkanPresentMode_.load(std::memory_order_acquire)
        ? virgl_ipc::kSurfaceVulkan : 0;
    if (!SendVirglTargetLocked(surfaceKey, producerWindow, framePeriodNs, flags)) return false;
    zeroCopyAttachedSurfaces_.insert(surfaceKey);
    return true;
}

void GraphicsBroker::SetZeroCopyFramePeriod(uint64_t surfaceKey, uint64_t framePeriodNs)
{
    std::lock_guard<std::mutex> lock(virglIpcMutex_);
    if (!surfaceKey || !framePeriodNs || !zeroCopyAttachedSurfaces_.count(surfaceKey)) return;
    if (!SendVirglFramePeriodLocked(surfaceKey, framePeriodNs))
    {
        OH_LOG_WARN(LOG_APP,
                    "[VIRGL-ZC][MAIN] frame period update failed key=%{public}llu period_ns=%{public}llu",
                    static_cast<unsigned long long>(surfaceKey),
                    static_cast<unsigned long long>(framePeriodNs));
    }
}

void GraphicsBroker::DetachZeroCopyTarget(uint64_t surfaceKey)
{
    std::lock_guard<std::mutex> lock(virglIpcMutex_);
    if (!surfaceKey || !zeroCopyAttachedSurfaces_.count(surfaceKey)) return;

    SendVirglDetachLocked(surfaceKey);
    zeroCopyAttachedSurfaces_.erase(surfaceKey);
    unlink(ZeroCopyReadyPath(surfaceKey).c_str());
    OH_LOG_INFO(LOG_APP, "[VIRGL-ZC][MAIN] detached surface_key=%{public}llu",
                static_cast<unsigned long long>(surfaceKey));
}

bool GraphicsBroker::QueryZeroCopySurfaces(std::vector<ZeroCopySurfaceInfo>& surfaces) const
{
    surfaces.clear();
    std::lock_guard<std::mutex> lock(virglIpcMutex_);
    if (!virglRemoteProxy_ || !virglIpcConfigured_) return false;

    OHIPCParcel* request = OH_IPCParcel_Create();
    OHIPCParcel* reply = OH_IPCParcel_Create();
    int32_t result = request
        ? OH_IPCParcel_WriteInt32(request, virgl_ipc::kProtocolVersion)
        : OH_IPC_MEM_ALLOCATOR_ERROR;
    if (result == OH_IPC_SUCCESS && reply)
    {
        OH_IPC_MessageOption option = {OH_IPC_REQUEST_MODE_SYNC, 0, nullptr};
        result = OH_IPCRemoteProxy_SendRequest(
            virglRemoteProxy_, virgl_ipc::kQuerySurfacesRequest,
            request, reply, &option);
    }

    virgl_ipc::SurfaceQueryReply queryReply;
    if (result == OH_IPC_SUCCESS)
    {
        const uint8_t* bytes = OH_IPCParcel_ReadBuffer(
            reply, static_cast<int32_t>(sizeof(queryReply)));
        if (!bytes)
        {
            result = OH_IPC_PARCEL_READ_ERROR;
        }
        else
        {
            memcpy(&queryReply, bytes, sizeof(queryReply));
            if (queryReply.magic != virgl_ipc::kMagic ||
                queryReply.version != static_cast<uint32_t>(virgl_ipc::kProtocolVersion) ||
                queryReply.size != sizeof(queryReply) ||
                queryReply.count > virgl_ipc::kMaxSurfaces)
                result = OH_IPC_PARCEL_READ_ERROR;
        }
    }

    if (result == OH_IPC_SUCCESS)
    {
        surfaces.reserve(queryReply.count);
        for (uint32_t i = 0; i < queryReply.count; ++i)
        {
            const auto& item = queryReply.surfaces[i];
            surfaces.push_back({item.surfaceKey, item.clientPid, item.surfaceId,
                                item.width, item.height, item.serial,
                                (item.flags & virgl_ipc::kSurfaceAttached) != 0});
        }
    }
    if (reply) OH_IPCParcel_Destroy(reply);
    if (request) OH_IPCParcel_Destroy(request);
    return result == OH_IPC_SUCCESS;
}

void GraphicsBroker::SetZeroCopySurfaceReady(uint64_t surfaceKey, bool ready)
{
    if (!surfaceKey) return;
    const std::string path = ZeroCopyReadyPath(surfaceKey);
    if (!ready)
    {
        unlink(path.c_str());
        return;
    }

    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0)
    {
        OH_LOG_WARN(LOG_APP,
                    "[VIRGL-ZC][MAIN] ready marker create failed key=%{public}llu errno=%{public}d",
                    static_cast<unsigned long long>(surfaceKey), errno);
        return;
    }
    static constexpr char payload[] = "ready\n";
    const ssize_t written = write(fd, payload, sizeof(payload) - 1);
    const int writeError = written == static_cast<ssize_t>(sizeof(payload) - 1)
        ? 0 : (errno ? errno : EIO);
    close(fd);
    if (writeError)
        OH_LOG_WARN(LOG_APP,
                    "[VIRGL-ZC][MAIN] ready marker write failed key=%{public}llu errno=%{public}d",
                    static_cast<unsigned long long>(surfaceKey), writeError);
}

void GraphicsBroker::ShutdownVirglIpc()
{
    std::lock_guard<std::mutex> lock(virglIpcMutex_);
    virglIpcAcceptCallback_ = false;
    if (virglRemoteProxy_)
    {
        OHIPCParcel* request = OH_IPCParcel_Create();
        OHIPCParcel* reply = OH_IPCParcel_Create();
        if (request && reply &&
            OH_IPCParcel_WriteInt32(request, virgl_ipc::kProtocolVersion) == OH_IPC_SUCCESS)
        {
            OH_IPC_MessageOption option = {OH_IPC_REQUEST_MODE_SYNC, 0, nullptr};
            OH_IPCRemoteProxy_SendRequest(
                virglRemoteProxy_, virgl_ipc::kShutdownRequest,
                request, reply, &option);
        }
        if (reply) OH_IPCParcel_Destroy(reply);
        if (request) OH_IPCParcel_Destroy(request);
        OH_IPCRemoteProxy_Destroy(virglRemoteProxy_);
        virglRemoteProxy_ = nullptr;
    }
    virglIpcConfigured_ = false;
    virglIpcCallbackComplete_ = false;
    for (uint64_t surfaceKey : zeroCopyAttachedSurfaces_)
        unlink(ZeroCopyReadyPath(surfaceKey).c_str());
    zeroCopyAttachedSurfaces_.clear();
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

    if (!started_) RemoveStaleZeroCopyMarkers();

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
    bool serverUsesIpc = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        virglServerRunning_.store(false, std::memory_order_release);
        socketPath = virglSocketPath_;
        serverPid = virglServerPid_;
        serverUsesNcp = virglServerUsesNcp_;
        serverUsesIpc = virglServerUsesIpc_;
        virglServerPid_ = -1;
        virglServerUsesNcp_ = false;
        virglServerUsesIpc_ = false;
        virglSocketReady_ = false;
        activeBackend_ = GraphicsBackend::Shm;
        started_ = false;
        runtimeReady_ = false;
    }

    if (serverUsesIpc)
    {
        ShutdownVirglIpc();
    }
    else if (serverPid > 0)
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

void GraphicsBroker::SetVulkanPresentMode(bool enabled)
{
    vulkanPresentMode_.store(enabled, std::memory_order_release);
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
    {
        std::lock_guard<std::mutex> ipcLock(virglIpcMutex_);
        state.zeroCopyFramePath = state.active == GraphicsBackend::Virgl && virglIpcConfigured_;
    }
    state.runtimeDir = runtimeDir_;
    state.virglSocketPath = virglSocketPath_;
    state.virglLibraryPath = virglLibraryPath_;
    state.frameTransportMode = state.zeroCopyFramePath
        ? "virgl_texture+surface_queue+external_oes"
        : "wl_shm+cpu_copy+gl_upload";
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
    env.push_back(std::string("WINEHUA_FRAME_ZERO_COPY=") + (state.zeroCopyFramePath ? "1" : "0"));
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
    if (vulkanPresentMode_.load(std::memory_order_acquire))
        env.push_back("WINEHUA_VULKAN_PRESENT=1");
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
        env.push_back("WINEHUA_VTEST_FRONTBUFFER_LOG=/data/storage/el2/base/temp/winehua_vtest_frontbuffer.log");
        env.push_back("WINEHUA_VTEST_PRESENT=surface-queue");
        env.push_back(std::string("WINEHUA_ZERO_COPY_READY_DIR=") + ZERO_COPY_READY_DIR);
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

    const std::string nextRuntimeDir = runtimeDir + "/graphics";
    runtimeDir_ = nextRuntimeDir;
    virglSocketPath_ = runtimeDir_ + "/virgl.sock";
    runtimeReady_ = EnsureDir(runtimeDir_);
    return runtimeReady_;
}

bool GraphicsBroker::IsVirglServerProcessAliveLocked()
{
    if (virglServerUsesIpc_)
    {
        std::lock_guard<std::mutex> lock(virglIpcMutex_);
        bool responsive = false;
        if (virglRemoteProxy_ && OH_IPCRemoteProxy_IsRemoteDead(virglRemoteProxy_) == 0)
        {
            OHIPCParcel* request = OH_IPCParcel_Create();
            OHIPCParcel* reply = OH_IPCParcel_Create();
            int32_t result = request
                ? OH_IPCParcel_WriteInt32(request, virgl_ipc::kProtocolVersion)
                : OH_IPC_MEM_ALLOCATOR_ERROR;
            if (result == OH_IPC_SUCCESS && reply)
            {
                OH_IPC_MessageOption option = {OH_IPC_REQUEST_MODE_SYNC, 0, nullptr};
                result = OH_IPCRemoteProxy_SendRequest(
                    virglRemoteProxy_, virgl_ipc::kQuerySurfacesRequest,
                    request, reply, &option);
            }
            responsive = result == OH_IPC_SUCCESS;
            if (reply) OH_IPCParcel_Destroy(reply);
            if (request) OH_IPCParcel_Destroy(request);
        }
        if (responsive) return true;

        lastError_ = "virgl IPC native child process is not responding";
        if (virglRemoteProxy_)
        {
            OH_IPCRemoteProxy_Destroy(virglRemoteProxy_);
            virglRemoteProxy_ = nullptr;
        }
        virglIpcConfigured_ = false;
        virglIpcCallbackComplete_ = false;
        virglServerUsesIpc_ = false;
        virglServerRunning_.store(false, std::memory_order_release);
        virglSocketReady_ = false;
        return false;
    }
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
    std::string serverDir;
    std::string ldLibraryPath;

    if (!runtimeReady_ || virglSocketPath_.empty()) return;
    if (virglServerRunning_.load(std::memory_order_acquire) && IsVirglServerProcessAliveLocked()) return;
    if (wineRuntimeBinDir_.empty())
    {
        lastError_ = "wine runtime bin dir is not configured; using shm fallback";
        return;
    }

    if (virglVtestLibraryPath_.empty() || !FileExists(virglVtestLibraryPath_))
    {
        lastError_ = "virgl vtest helper is missing from the bundle";
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
        const std::string virglLogPath = "/data/storage/el2/base/cache/winehua_virgl_host.log";
        const char* requestedShadowMode = getenv("VKR_WINEHUA_SHADOW_FROM_HOST");
        const char* requestedShadowTrace = getenv("VKR_WINEHUA_SHADOW_TRACE");
        const std::string shadowMode = requestedShadowMode && requestedShadowMode[0]
            ? requestedShadowMode : "full";
        const std::string shadowTrace = requestedShadowTrace && requestedShadowTrace[0]
            ? requestedShadowTrace : "0";
        {
            std::lock_guard<std::mutex> ipcLock(virglIpcMutex_);
            virglIpcHelperPath_ = virglVtestLibraryPath_;
            virglIpcSocketPath_ = virglSocketPath_;
            virglIpcLibraryPath_ = ldLibraryPath;
            virglIpcSyncMode_ = syncMode;
            virglIpcLogPath_ = virglLogPath;
            virglIpcShadowMode_ = shadowMode;
            virglIpcShadowTrace_ = shadowTrace;
            virglIpcAcceptCallback_ = true;
            virglIpcCallbackComplete_ = false;
            virglIpcConfigured_ = false;
            virglIpcError_ = 0;
        }

        const int32_t ret = OH_Ability_CreateNativeChildProcess(
            "libvirgl_child.so", &GraphicsBroker::OnVirglIpcProcessStarted);
        if (ret != NCP_NO_ERROR)
        {
            std::lock_guard<std::mutex> ipcLock(virglIpcMutex_);
            virglIpcAcceptCallback_ = false;
            lastError_ = "failed to create virgl IPC native child process ret=" + std::to_string(ret);
            virglServerRunning_.store(false, std::memory_order_release);
            virglSocketReady_ = false;
            return;
        }

        std::unique_lock<std::mutex> ipcLock(virglIpcMutex_);
        const bool callbackCompleted = virglIpcCondition_.wait_for(
            ipcLock, std::chrono::seconds(5), [this]() { return virglIpcCallbackComplete_; });
        if (!callbackCompleted || !virglIpcConfigured_)
        {
            virglIpcAcceptCallback_ = false;
            lastError_ = callbackCompleted
                ? "failed to configure virgl IPC native child process ret=" + std::to_string(virglIpcError_)
                : "timed out waiting for virgl IPC native child process";
            ipcLock.unlock();
            ShutdownVirglIpc();
            virglServerRunning_.store(false, std::memory_order_release);
            virglSocketReady_ = false;
            return;
        }
        ipcLock.unlock();

        virglServerUsesIpc_ = true;
        virglServerUsesNcp_ = false;
        OH_LOG_INFO(LOG_APP,
                    "[GraphicsBroker] IPC NCP virgl_child configured helper=%{public}s "
                    "socket=%{public}s hostLib=%{public}s sync=%{public}s log=%{public}s",
                    virglVtestLibraryPath_.c_str(), virglSocketPath_.c_str(),
                    ldLibraryPath.c_str(), syncMode.c_str(), virglLogPath.c_str());
    }

    virglServerPid_ = -1;
    virglServerRunning_.store(true, std::memory_order_release);
    virglSocketReady_ = false;

    OH_LOG_INFO(LOG_APP, "[GraphicsBroker] waiting for virgl socket at %{public}s",
                virglSocketPath_.c_str());

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
    if (virglServerUsesIpc_) ShutdownVirglIpc();
    virglServerPid_ = -1;
    virglServerUsesNcp_ = false;
    virglServerUsesIpc_ = false;
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
