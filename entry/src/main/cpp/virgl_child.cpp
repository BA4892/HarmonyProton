#include <AbilityKit/native_child_process.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <IPCKit/ipc_kit.h>
#include <hilog/log.h>
#include <native_window/external_window.h>

#include "virgl_ipc_protocol.h"
#include "virgl_surface_presenter.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <dlfcn.h>
#include <mutex>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "virgl-child"

namespace {

using WinehuaVtestMain = int (*)(int argc, char** argv);
using WinehuaVtestPresentCallback = int (*)(
    uint32_t texId, uint32_t width, uint32_t height, uint32_t format,
    uint32_t resourceFlags, uint64_t drawable, uint32_t serial,
    uint32_t clientPid, uint32_t surfaceId, uint32_t presentFlags,
    uint64_t* nextPresentDeadlineNs, void* userData);
using WinehuaVtestSetPresentCallback = void (*)(
    WinehuaVtestPresentCallback callback, void* userData);
using WinehuaVtestReleaseQueueCallback = void (*)(void* queueSyncData);
using WinehuaVtestVulkanPresentCallback = int (*)(
    uint32_t contextId, uintptr_t instance, uintptr_t physicalDevice,
    uintptr_t device, uintptr_t queue, uint64_t image, uint32_t queueFamily,
    uint32_t width, uint32_t height, uint32_t format, uint32_t layout,
    uint32_t clientPid, uint32_t surfaceId, uint32_t serial,
    uint32_t presentFlags, uint64_t* nextPresentDeadlineNs,
    WinehuaVtestReleaseQueueCallback releaseQueue, void* queueSyncData,
    void* userData);
using WinehuaVtestSetVulkanPresentCallback = void (*)(
    WinehuaVtestVulkanPresentCallback callback, void* userData);
enum class IpcChildMode {
    None,
    VtestServer,
};

struct VtestIpcConfig {
    std::string helperPath;
    std::string socketPath;
    std::string libraryPath;
    std::string syncMode;
    std::string logPath;
    std::string shadowMode;
    std::string shadowTrace;
    std::string presentMode;
};

std::mutex g_ipcChildMutex;
std::condition_variable g_ipcChildCondition;
IpcChildMode g_ipcChildMode = IpcChildMode::None;
VtestIpcConfig g_vtestIpcConfig;
OHIPCRemoteStub* g_virglIpcStub = nullptr;

void ForwardPerfSummary(const std::string& path, std::atomic<bool>& stop)
{
    FILE* file = nullptr;
    char line[4096];
    while (!stop.load(std::memory_order_relaxed))
    {
        if (!file)
        {
            file = fopen(path.c_str(), "r");
            if (!file)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                continue;
            }
            fseek(file, 0, SEEK_END);
        }

        bool forwarded = false;
        while (fgets(line, sizeof(line), file))
        {
            if (!strstr(line, "WineHuaPerf")) continue;
            line[strcspn(line, "\r\n")] = '\0';
            OH_LOG_INFO(LOG_APP, "[VIRGL-PERF] %{public}s", line);
            forwarded = true;
        }
        if (feof(file)) clearerr(file);
        if (!forwarded)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (file) fclose(file);
}

int WriteIpcResult(OHIPCParcel* reply, int32_t result)
{
    return reply ? OH_IPCParcel_WriteInt32(reply, result) : OH_IPC_CHECK_PARAM_ERROR;
}

int OnVirglIpcRequest(uint32_t code, const OHIPCParcel* data,
                      OHIPCParcel* reply, void*)
{
    int32_t version = 0;
    int32_t result = !data || OH_IPCParcel_ReadInt32(data, &version) != OH_IPC_SUCCESS ||
        version != winehua::virgl_ipc::kProtocolVersion ? -1 : 0;

    if (result == 0 && code == winehua::virgl_ipc::kQuerySurfacesRequest)
    {
        const auto queryReply = winehua::QueryVirglSurfaces();
        return reply
            ? OH_IPCParcel_WriteBuffer(
                  reply, reinterpret_cast<const uint8_t*>(&queryReply),
                  static_cast<int32_t>(sizeof(queryReply)))
            : OH_IPC_CHECK_PARAM_ERROR;
    }

    if (result == 0 && code == winehua::virgl_ipc::kConfigureRequest)
    {
        const char* helperPath = OH_IPCParcel_ReadString(data);
        const char* socketPath = OH_IPCParcel_ReadString(data);
        const char* libraryPath = OH_IPCParcel_ReadString(data);
        const char* syncMode = OH_IPCParcel_ReadString(data);
        const char* logPath = OH_IPCParcel_ReadString(data);
        const char* shadowMode = OH_IPCParcel_ReadString(data);
        const char* shadowTrace = OH_IPCParcel_ReadString(data);
        const char* presentMode = OH_IPCParcel_ReadString(data);
        if (!helperPath || helperPath[0] != '/' || !socketPath || socketPath[0] != '/' ||
            !libraryPath || libraryPath[0] != '/' || !syncMode || !logPath || logPath[0] != '/' ||
            !shadowMode || !shadowTrace || !presentMode ||
            (strcmp(presentMode, "fifo") && strcmp(presentMode, "mailbox") &&
             strcmp(presentMode, "fifo-async") &&
             strcmp(presentMode, "fifo-poll")))
        {
            result = -2;
        }
        else
        {
            std::lock_guard<std::mutex> lock(g_ipcChildMutex);
            if (g_ipcChildMode != IpcChildMode::None)
            {
                result = -3;
            }
            else
            {
                g_vtestIpcConfig.helperPath = helperPath;
                g_vtestIpcConfig.socketPath = socketPath;
                g_vtestIpcConfig.libraryPath = libraryPath;
                g_vtestIpcConfig.syncMode = syncMode;
                g_vtestIpcConfig.logPath = logPath;
                g_vtestIpcConfig.shadowMode = shadowMode;
                g_vtestIpcConfig.shadowTrace = shadowTrace;
                g_vtestIpcConfig.presentMode = presentMode;
                g_ipcChildMode = IpcChildMode::VtestServer;
            }
        }
        g_ipcChildCondition.notify_all();
        OH_LOG_INFO(LOG_APP,
                    "[VIRGL-ZC][NCP] configure result=%{public}d helper=%{public}s socket=%{public}s",
                    result, helperPath ? helperPath : "(null)",
                    socketPath ? socketPath : "(null)");
    }
    else if (result == 0 && code == winehua::virgl_ipc::kAttachSurfaceRequest)
    {
        int64_t surfaceKey = 0;
        int64_t framePeriodNs = 0;
        int32_t flags = 0;
        OHNativeWindow* window = nullptr;
        if (OH_IPCParcel_ReadInt64(data, &surfaceKey) != OH_IPC_SUCCESS || surfaceKey <= 0 ||
            OH_IPCParcel_ReadInt64(data, &framePeriodNs) != OH_IPC_SUCCESS || framePeriodNs <= 0 ||
            OH_IPCParcel_ReadInt32(data, &flags) != OH_IPC_SUCCESS || flags < 0 ||
            OH_NativeWindow_ReadFromParcel(const_cast<OHIPCParcel*>(data), &window) != 0 || !window)
        {
            if (window) OH_NativeWindow_DestroyNativeWindow(window);
            result = -4;
        }
        else
        {
            result = winehua::AttachVirglSurfaceTarget(
                static_cast<uint64_t>(surfaceKey),
                static_cast<uint64_t>(framePeriodNs),
                static_cast<uint32_t>(flags), window);
            if (result != 0) OH_NativeWindow_DestroyNativeWindow(window);
        }
    }
    else if (result == 0 && code == winehua::virgl_ipc::kDetachSurfaceRequest)
    {
        int64_t surfaceKey = 0;
        result = OH_IPCParcel_ReadInt64(data, &surfaceKey) == OH_IPC_SUCCESS && surfaceKey > 0
            ? winehua::DetachVirglSurfaceTarget(static_cast<uint64_t>(surfaceKey)) : -5;
    }
    else if (result == 0 && code == winehua::virgl_ipc::kSetFramePeriodRequest)
    {
        int64_t surfaceKey = 0;
        int64_t framePeriodNs = 0;
        result = OH_IPCParcel_ReadInt64(data, &surfaceKey) == OH_IPC_SUCCESS &&
            OH_IPCParcel_ReadInt64(data, &framePeriodNs) == OH_IPC_SUCCESS &&
            surfaceKey > 0 && framePeriodNs > 0
            ? winehua::SetVirglSurfaceFramePeriod(
                  static_cast<uint64_t>(surfaceKey),
                  static_cast<uint64_t>(framePeriodNs))
            : -7;
    }
    else if (result == 0 && code == winehua::virgl_ipc::kShutdownRequest)
    {
        winehua::ResetVirglSurfaces();
        std::thread([]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            _exit(0);
        }).detach();
    }
    else if (result == 0)
    {
        result = -6;
    }

    return WriteIpcResult(reply, result);
}

bool IsAllowedHostEnv(const std::string& key)
{
    return key == "LD_LIBRARY_PATH" ||
           key == "VTEST_USE_GLES" ||
           key == "VTEST_USE_EGL_SURFACELESS" ||
           key == "VTEST_SYNC_GL_FINISH" ||
           key == "VIRGL_DISABLE_NATIVE_FENCE_FD" ||
           key == "WINEHUA_VIRGL_SYNC_MODE" ||
           key == "WINEHUA_VIRGL_LOG_PATH" ||
           key == "VKR_WINEHUA_SHADOW_TO_HOST" ||
           key == "WINEHUA_VKR_TRACE_SAMPLED" ||
           key == "WINEHUA_VKR_TRACE_PIPELINE" ||
           key == "WINEHUA_VKR_TRACE_CAPTURE" ||
           key == "WINEHUA_VKR_TRACE_CAPTURE_LIMIT" ||
           key == "WINEHUA_VKR_TRACE_UBO_IDENTITY" ||
           key == "WINEHUA_VKR_TRACE_PRESENT_IMAGE" ||
           key == "VKR_WINEHUA_SHADOW_FROM_HOST" ||
           key == "VKR_WINEHUA_SHADOW_TRACE" ||
           key == "WINEHUA_VKR_PRESENT_STAGE_TRACE" ||
           key == "VKR_WINEHUA_PERF_SUMMARY" ||
           key == "VKR_WINEHUA_GPU_UPLOAD" ||
           key == "VKR_WINEHUA_GPU_UPLOAD_INLINE" ||
           key == "VKR_WINEHUA_GPU_UPLOAD_SERIALIZE" ||
           key == "VKR_WINEHUA_SHADOW_GENERATION_SERIALIZE" ||
           key == "VKR_WINEHUA_DESCRIPTOR_UPDATE_SERIALIZE" ||
           key == "VKR_WINEHUA_SHADOW_DIRTY_LIST" ||
           key == "VKR_WINEHUA_BOUND_BUFFER_LIST" ||
           key == "VKR_WINEHUA_BATCH_FLUSH" ||
           key == "VKR_WINEHUA_SHADOW_MSYNC" ||
           key == "VKR_WINEHUA_SHADOW_SUBMIT_UNMAP_LARGE" ||
           key == "VKR_WINEHUA_SHADOW_MERGE_RANGES" ||
           key == "WINEHUA_VENUS_PRESENT_MODE" ||
           key == "WINEHUA_VKR_FREEZE_BOOL_SPEC" ||
           key == "EGL_PLATFORM";
}

void ClearGuestGraphicsEnv()
{
    const char* keys[] = {
        "GALLIUM_DRIVER",
        "MESA_LOADER_DRIVER_OVERRIDE",
        "LIBGL_ALWAYS_SOFTWARE",
        "LIBGL_DRIVERS_PATH",
        "EGL_DRIVERS_PATH",
        "__EGL_VENDOR_LIBRARY_DIRS",
        "BOX64_LD_LIBRARY_PATH",
        "BOX64_EMULATED_LIBS",
        "VTEST_SOCKET_NAME",
    };

    for (const char* key : keys) unsetenv(key);
}

void ApplyHostEnv(const char* token)
{
    static const char prefix[] = "__env=";
    const char* assignment;
    const char* equals;
    std::string key;

    if (!token || strncmp(token, prefix, sizeof(prefix) - 1)) return;
    assignment = token + sizeof(prefix) - 1;
    equals = strchr(assignment, '=');
    if (!equals || equals == assignment) return;

    key.assign(assignment, static_cast<size_t>(equals - assignment));
    if (!IsAllowedHostEnv(key))
    {
        OH_LOG_WARN(LOG_APP, "[virgl-child] rejected env key=%{public}s", key.c_str());
        return;
    }
    setenv(key.c_str(), equals + 1, 1);
}

int OnVtestPresent(uint32_t texId, uint32_t width, uint32_t height,
                   uint32_t format, uint32_t resourceFlags,
                   uint64_t drawable, uint32_t serial,
                   uint32_t clientPid, uint32_t surfaceId,
                   uint32_t presentFlags, uint64_t* nextPresentDeadlineNs, void*)
{
    static std::atomic<uint64_t> callCount{0};
    const uint64_t call = callCount.fetch_add(1, std::memory_order_relaxed) + 1;
    const EGLDisplay display = eglGetCurrentDisplay();
    const EGLContext context = eglGetCurrentContext();
    const bool textureVisible = display != EGL_NO_DISPLAY &&
        context != EGL_NO_CONTEXT && texId != 0 && glIsTexture(texId) == GL_TRUE;
    if (nextPresentDeadlineNs) *nextPresentDeadlineNs = 0;
    const int presentResult = textureVisible
        ? winehua::PresentVirglSurface(
              clientPid, surfaceId, texId, width, height, drawable, serial,
              nextPresentDeadlineNs)
        : -1;

    if (call == 1 || !textureVisible ||
        (presentResult < -2 && presentResult != -6))
    {
        OH_LOG_INFO(LOG_APP,
                    "[VIRGL-PRESENT][NCP] call=%{public}llu serial=%{public}u "
                    "pid=%{public}u surface=%{public}u drawable=0x%{public}llx tex=%{public}u visible=%{public}s "
                    "size=%{public}ux%{public}u format=%{public}u "
                    "resource_flags=0x%{public}x present_flags=0x%{public}x "
                    "display=%{public}p context=%{public}p blit=%{public}d "
                    "deadline_ns=%{public}llu",
                    static_cast<unsigned long long>(call), serial, clientPid, surfaceId,
                    static_cast<unsigned long long>(drawable), texId,
                    textureVisible ? "PASS" : "FAIL", width, height, format,
                    resourceFlags, presentFlags, display, context, presentResult,
                    static_cast<unsigned long long>(
                        nextPresentDeadlineNs ? *nextPresentDeadlineNs : 0));
    }
    return presentResult;
}

int OnVtestVulkanPresent(uint32_t contextId,
                         uintptr_t instance,
                         uintptr_t physicalDevice,
                         uintptr_t device,
                         uintptr_t queue,
                         uint64_t image,
                         uint32_t queueFamily,
                         uint32_t width,
                         uint32_t height,
                         uint32_t format,
                         uint32_t layout,
                         uint32_t clientPid,
                         uint32_t surfaceId,
                         uint32_t serial,
                         uint32_t presentFlags,
                         uint64_t* nextPresentDeadlineNs,
                         WinehuaVtestReleaseQueueCallback releaseQueue,
                         void* queueSyncData,
                         void*)
{
    static std::atomic<uint64_t> callCount{0};
    static std::atomic<uint64_t> successCount{0};
    static std::atomic<uint64_t> retryCount{0};
    static std::atomic<uint64_t> failureCount{0};
    const uint64_t call = callCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (nextPresentDeadlineNs) *nextPresentDeadlineNs = 0;
    if (presentFlags) return -1;
    const int result = winehua::PresentVenusSurface(
        contextId, instance, physicalDevice, device, queue, image,
        queueFamily, width, height, format, layout, clientPid, surfaceId,
        serial, nextPresentDeadlineNs, releaseQueue, queueSyncData);
    uint64_t failures = failureCount.load(std::memory_order_relaxed);
    if (result == 0) {
        successCount.fetch_add(1, std::memory_order_relaxed);
    } else if (result > 0) {
        retryCount.fetch_add(1, std::memory_order_relaxed);
    } else {
        failures = failureCount.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    if (call == 1 || !(call % 120) ||
        (result < 0 && (failures == 1 || !(failures % 60)))) {
        OH_LOG_INFO(LOG_APP,
                    "[VENUS-PRESENT][NCP] callback count=%{public}llu ok=%{public}llu "
                    "retry=%{public}llu fail=%{public}llu ctx=%{public}u serial=%{public}u "
                    "pid=%{public}u surface=%{public}u key=%{public}llu "
                    "size=%{public}ux%{public}u result=%{public}d",
                    static_cast<unsigned long long>(call),
                    static_cast<unsigned long long>(
                        successCount.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(
                        retryCount.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(failures), contextId, serial,
                    clientPid, surfaceId,
                    static_cast<unsigned long long>(
                        (static_cast<uint64_t>(clientPid) << 32) | surfaceId),
                    width, height, result);
    }
    return result;
}

} // namespace

extern "C" __attribute__((visibility("default"))) void Main(NativeChildProcess_Args args);

extern "C" __attribute__((visibility("default"))) OHIPCRemoteStub* NativeChildProcess_OnConnect()
{
    g_virglIpcStub = OH_IPCRemoteStub_Create(
        "winehua.virgl.Runtime", OnVirglIpcRequest, nullptr, nullptr);
    OH_LOG_INFO(LOG_APP, "[VIRGL-ZC][NCP] ipc_connect stub=%{public}s pid=%{public}d",
                g_virglIpcStub ? "PASS" : "FAIL", getpid());
    return g_virglIpcStub;
}

extern "C" __attribute__((visibility("default"))) void NativeChildProcess_MainProc()
{
    ClearGuestGraphicsEnv();
    setenv("EGL_PLATFORM", "surfaceless", 1);
    std::unique_lock<std::mutex> lock(g_ipcChildMutex);
    const bool completed = g_ipcChildCondition.wait_for(
        lock, std::chrono::seconds(5), [] { return g_ipcChildMode != IpcChildMode::None; });
    const IpcChildMode mode = g_ipcChildMode;
    const VtestIpcConfig config = g_vtestIpcConfig;
    lock.unlock();

    if (completed && mode == IpcChildMode::VtestServer)
    {
        const bool explicitToHost = config.shadowMode == "to-host-explicit";
        const bool preciseDirty = config.shadowMode == "precise-dirty";
        const bool precise = config.shadowMode == "precise" || preciseDirty;
        const bool frameAssocTrace =
            config.shadowTrace == "inline-gpu-upload-frame-assoc-trace";
        const bool presentImageTrace = config.shadowTrace == "present-image-trace";
        const bool captureTrace = config.shadowTrace == "1" || frameAssocTrace;
        const bool noGpuUploadFast =
            config.shadowTrace == "no-gpu-upload-fast";
        const bool noGpuUpload = config.shadowTrace == "no-gpu-upload" ||
            noGpuUploadFast;
        const bool serializedGpuUpload =
            config.shadowTrace == "inline-gpu-upload-serialized";
        const char* descriptorEnv = getenv("VKR_WINEHUA_DESCRIPTOR_UPDATE_SERIALIZE");
        const bool descriptorSerialized =
            (descriptorEnv && descriptorEnv[0] == 49 && !descriptorEnv[1]) ||
            config.shadowTrace == "inline-gpu-upload-descriptor-serialized";
        const bool inlineGpuUpload =
            config.shadowTrace == "inline-gpu-upload" || serializedGpuUpload ||
            descriptorSerialized || frameAssocTrace;
        const bool perfSummary = config.shadowTrace == "perf" ||
            config.shadowTrace == "no-gpu-upload" || inlineGpuUpload ||
            descriptorSerialized;
        const bool boundBufferList = config.shadowTrace == "perf";
        const bool cpuShadowUpload = config.shadowTrace == "cpu-upload";
        const bool legacyHostSync = config.shadowTrace == "legacy-host-sync";
        const char* mergeRanges = getenv("VKR_WINEHUA_SHADOW_MERGE_RANGES");
        if (!mergeRanges || !mergeRanges[0]) mergeRanges = "1";
        const std::string fromHostMode = explicitToHost ? "full" :
            (precise ? "precise" : config.shadowMode);
        const std::string toHostMode = explicitToHost || (precise && !preciseDirty)
            ? "explicit" : "full";
        const std::string sampledTrace = precise && !frameAssocTrace ? "0" : "1";
        std::string entryParams = config.helperPath + "|" + config.socketPath +
            "|__env=LD_LIBRARY_PATH=" + config.libraryPath +
            "|__env=VTEST_USE_GLES=1" +
            "|__env=VTEST_USE_EGL_SURFACELESS=1" +
            "|__env=VTEST_SYNC_GL_FINISH=1" +
            "|__env=WINEHUA_VIRGL_SYNC_MODE=" + config.syncMode +
           "|__env=WINEHUA_VIRGL_LOG_PATH=" + config.logPath +
           "|__env=WINEHUA_VKR_TRACE_SAMPLED=" + sampledTrace +
           "|__env=WINEHUA_VKR_TRACE_CAPTURE=" +
               (captureTrace ? "1" : "0") +
           "|__env=WINEHUA_VKR_TRACE_CAPTURE_LIMIT=" +
               (captureTrace ? "20000" : "512") +
           "|__env=WINEHUA_VKR_TRACE_UBO_IDENTITY=" +
               (frameAssocTrace ? "focused" : "0") +
           "|__env=WINEHUA_VKR_TRACE_PRESENT_IMAGE=" +
               (presentImageTrace ? "1" : "0") +
           "|__env=WINEHUA_VKR_TRACE_PIPELINE=" +
               (captureTrace ? "1" : "0") +
           "|__env=VKR_WINEHUA_SHADOW_FROM_HOST=" + fromHostMode +
            "|__env=VKR_WINEHUA_SHADOW_TO_HOST=" + toHostMode +
            "|__env=VKR_WINEHUA_SHADOW_TRACE=" +
               (captureTrace ? "1" : "0") +
            "|__env=VKR_WINEHUA_PERF_SUMMARY=" +
               (perfSummary ? "1" : "0") +
            "|__env=VKR_WINEHUA_GPU_UPLOAD=" +
               (noGpuUpload || cpuShadowUpload || legacyHostSync ? "0" : (captureTrace ? "1" : "auto")) +
            "|__env=VKR_WINEHUA_GPU_UPLOAD_INLINE=" +
               (inlineGpuUpload ? "1" : "0") +
            "|__env=VKR_WINEHUA_GPU_UPLOAD_SERIALIZE=" +
               (serializedGpuUpload ? "1" : "0") +
            "|__env=VKR_WINEHUA_SHADOW_GENERATION_SERIALIZE=" +
               (frameAssocTrace ? "1" : "0") +
            "|__env=VKR_WINEHUA_DESCRIPTOR_UPDATE_SERIALIZE=" +
               (descriptorSerialized ? "1" : "0") +
            "|__env=VKR_WINEHUA_SHADOW_DIRTY_LIST=" +
               (legacyHostSync ? "0" : "1") +
            "|__env=VKR_WINEHUA_BOUND_BUFFER_LIST=" +
               (boundBufferList ? "1" : "0") +
            "|__env=VKR_WINEHUA_BATCH_FLUSH=" +
               (legacyHostSync || noGpuUploadFast ? "0" : "1") +
            "|__env=VKR_WINEHUA_SHADOW_MERGE_RANGES=" + mergeRanges +
            "|__env=WINEHUA_VKR_PRESENT_STAGE_TRACE=" +
               (captureTrace ? "1" : "0") +
            "|__env=WINEHUA_VENUS_PRESENT_MODE=" + config.presentMode +
            "|__env=EGL_PLATFORM=surfaceless";
        if (config.syncMode == "egl-thread")
            entryParams += "|__env=VIRGL_DISABLE_NATIVE_FENCE_FD=1";
        NativeChildProcess_Args args = {};
        args.entryParams = entryParams.data();
        OH_LOG_INFO(LOG_APP, "[VIRGL-ZC][NCP] starting persistent vtest server");
        std::atomic<bool> perfLogStop{false};
        std::thread perfLogThread;
        if (perfSummary)
            perfLogThread = std::thread(ForwardPerfSummary, config.logPath,
                                        std::ref(perfLogStop));
        Main(args);
        perfLogStop.store(true, std::memory_order_relaxed);
        if (perfLogThread.joinable()) perfLogThread.join();
        winehua::ResetVirglSurfaces();
    }
    else
    {
        OH_LOG_ERROR(LOG_APP,
                     "[VIRGL-ZC][NCP] IPC configuration %{public}s mode=%{public}d",
                     completed ? "rejected" : "timed out", static_cast<int>(mode));
    }
    if (g_virglIpcStub)
    {
        OH_IPCRemoteStub_Destroy(g_virglIpcStub);
        g_virglIpcStub = nullptr;
    }
}

extern "C" __attribute__((visibility("default"))) void Main(NativeChildProcess_Args args)
{
    const char* entryParams = args.entryParams ? args.entryParams : "";
    char* buffer = strdup(entryParams);
    char* save = nullptr;
    char* helperPath;
    char* socketPath;
    void* handle;
    WinehuaVtestMain vtestMain;
    WinehuaVtestSetPresentCallback setPresentCallback;
    WinehuaVtestSetVulkanPresentCallback setVulkanPresentCallback;

    OH_LOG_INFO(LOG_APP, "[virgl-child] Main enter pid=%{public}d params=%{public}s",
                getpid(), entryParams);
    if (!buffer)
    {
        OH_LOG_ERROR(LOG_APP, "[virgl-child] entryParams allocation failed");
        return;
    }

    helperPath = strtok_r(buffer, "|", &save);
    socketPath = strtok_r(nullptr, "|", &save);
    if (!helperPath || !socketPath)
    {
        OH_LOG_ERROR(LOG_APP, "[virgl-child] invalid entryParams");
        free(buffer);
        return;
    }

    ClearGuestGraphicsEnv();
    for (char* token = strtok_r(nullptr, "|", &save); token; token = strtok_r(nullptr, "|", &save))
        ApplyHostEnv(token);

    OH_LOG_INFO(LOG_APP,
                "[virgl-child] helper=%{public}s socket=%{public}s hostLib=%{public}s egl=%{public}s "
                "gles=%{public}s sync=%{public}s shadow=%{public}s trace=%{public}s "
                "present=%{public}s",
                helperPath, socketPath,
                getenv("LD_LIBRARY_PATH") ? getenv("LD_LIBRARY_PATH") : "(unset)",
                getenv("EGL_PLATFORM") ? getenv("EGL_PLATFORM") : "(unset)",
                getenv("VTEST_USE_GLES") ? getenv("VTEST_USE_GLES") : "(unset)",
                getenv("WINEHUA_VIRGL_SYNC_MODE") ? getenv("WINEHUA_VIRGL_SYNC_MODE") : "egl-thread",
                getenv("VKR_WINEHUA_SHADOW_FROM_HOST") ? getenv("VKR_WINEHUA_SHADOW_FROM_HOST") : "full",
                getenv("VKR_WINEHUA_SHADOW_TRACE") ? getenv("VKR_WINEHUA_SHADOW_TRACE") : "0",
                getenv("WINEHUA_VENUS_PRESENT_MODE")
                    ? getenv("WINEHUA_VENUS_PRESENT_MODE") : "fifo");

    handle = dlopen(helperPath, RTLD_NOW | RTLD_LOCAL);
    if (!handle)
    {
        OH_LOG_ERROR(LOG_APP, "[virgl-child] dlopen helper failed: %{public}s", dlerror());
        free(buffer);
        return;
    }

    vtestMain = reinterpret_cast<WinehuaVtestMain>(dlsym(handle, "winehua_vtest_main"));
    if (!vtestMain)
    {
        OH_LOG_ERROR(LOG_APP, "[virgl-child] winehua_vtest_main missing: %{public}s", dlerror());
        dlclose(handle);
        free(buffer);
        return;
    }

    setPresentCallback = reinterpret_cast<WinehuaVtestSetPresentCallback>(
        dlsym(handle, "winehua_vtest_set_present_callback"));
    if (setPresentCallback)
        setPresentCallback(OnVtestPresent, nullptr);
    else
        OH_LOG_WARN(LOG_APP, "[virgl-child] present callback registration missing: %{public}s",
                    dlerror());

    setVulkanPresentCallback = reinterpret_cast<WinehuaVtestSetVulkanPresentCallback>(
        dlsym(handle, "winehua_vtest_set_vulkan_present_callback"));
    if (setVulkanPresentCallback)
        setVulkanPresentCallback(OnVtestVulkanPresent, nullptr);
    else
        OH_LOG_WARN(LOG_APP,
                    "[virgl-child] Vulkan present callback registration missing: %{public}s",
                    dlerror());

    char arg0[] = "virgl_test_server";
    char arg1[] = "--no-fork";
    char arg2[] = "--multi-clients";
    char arg3[] = "--use-egl-surfaceless";
    char arg4[] = "--use-gles";
    char arg5[] = "--venus";
    char arg6[] = "--socket-path";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, socketPath, nullptr};
    int rc = vtestMain(8, argv);

    OH_LOG_WARN(LOG_APP, "[virgl-child] vtest exited rc=%{public}d", rc);
    if (setVulkanPresentCallback) setVulkanPresentCallback(nullptr, nullptr);
    if (setPresentCallback) setPresentCallback(nullptr, nullptr);
    dlclose(handle);
    free(buffer);
}
