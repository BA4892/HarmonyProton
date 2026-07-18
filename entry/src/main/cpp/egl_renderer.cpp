#include "egl_renderer.h"
#include "graphics_broker.h"
#include "wayland_server.h"
#include "fps_counter.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <vector>
#include <mutex>
#include <fcntl.h>
#include <native_vsync/native_vsync.h>
#include <native_buffer/native_buffer.h>
#include <native_image/native_image.h>
#include <GLES2/gl2ext.h>
#include <unistd.h>

#undef LOG_TAG
#define LOG_TAG "WL_EGL"
#include <hilog/log.h>

// -- 共享 EGLDisplay: 整个进程只初始化一次, 避免反复 init/terminate 导致 GPU 驱动竞争 --
static EGLDisplay gSharedDisplay = EGL_NO_DISPLAY;
static std::once_flag gDisplayOnce;

namespace {

using PerfClock = std::chrono::steady_clock;

static uint64_t PerfNowUs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        PerfClock::now().time_since_epoch()).count());
}

struct RendererPerfWindow {
    static constexpr size_t kSamples = 120;

    std::array<uint64_t, kSamples> takeUs{};
    std::array<uint64_t, kSamples> uploadUs{};
    std::array<uint64_t, kSamples> swapUs{};
    std::array<uint64_t, kSamples> totalUs{};
    size_t count = 0;
    uint64_t displayed = 0;
    uint64_t windowDisplayed = 0;
    uint64_t failedSwaps = 0;
    uint64_t uploadBytes = 0;
    uint64_t startedUs = PerfNowUs();
    uint64_t publishStartedUs = startedUs;
    uint64_t publishFrames = 0;
    uint64_t publishSequence = 0;

    void PublishDisplayedFps(uint32_t toplevelId, uint64_t nowUs)
    {
        static constexpr const char* kPath =
            "/data/storage/el2/base/files/.wine/drive_c/windows/temp/winehua_display_fps.txt";
        const uint64_t elapsedUs = nowUs - publishStartedUs;
        if (elapsedUs < 1000000) return;

        const double fps = static_cast<double>(publishFrames) * 1000000.0 /
                           static_cast<double>(std::max<uint64_t>(1, elapsedUs));
        char tempPath[192];
        char payload[128];
        const unsigned long long nextSequence =
            static_cast<unsigned long long>(publishSequence + 1);
        const int payloadLength = std::snprintf(
            payload, sizeof(payload), "%llu %.3f %u\n", nextSequence, fps, toplevelId);
        std::snprintf(tempPath, sizeof(tempPath), "%s.tmp.%d.%p",
                      kPath, getpid(), static_cast<void*>(this));

        const int fd = payloadLength > 0 && payloadLength < static_cast<int>(sizeof(payload))
            ? open(tempPath, O_WRONLY | O_CREAT | O_TRUNC, 0666) : -1;
        if (fd >= 0)
        {
            const ssize_t written = write(fd, payload, static_cast<size_t>(payloadLength));
            close(fd);
            if (written == payloadLength && !rename(tempPath, kPath))
                publishSequence++;
            else
                unlink(tempPath);
        }

        publishFrames = 0;
        publishStartedUs = nowUs;
    }

    static uint64_t Percentile(std::array<uint64_t, kSamples> values, size_t count,
                               unsigned int percentile)
    {
        std::sort(values.begin(), values.begin() + count);
        const size_t index = std::min(count - 1, (count * percentile + 99) / 100 - 1);
        return values[index];
    }

    void Add(uint32_t toplevelId, uint64_t take, uint64_t upload, uint64_t swap,
             uint64_t total, size_t bytes, bool swapOk)
    {
        takeUs[count] = take;
        uploadUs[count] = upload;
        swapUs[count] = swap;
        totalUs[count] = total;
        ++count;
        if (swapOk)
        {
            ++displayed;
            ++windowDisplayed;
            ++publishFrames;
        }
        uploadBytes += bytes;
        if (!swapOk) ++failedSwaps;

        const uint64_t nowUs = PerfNowUs();
        PublishDisplayedFps(toplevelId, nowUs);

        if (count != kSamples) return;

        const double fps = static_cast<double>(windowDisplayed) * 1000000.0 /
                           static_cast<double>(std::max<uint64_t>(1, nowUs - startedUs));
        OH_LOG_INFO(LOG_APP,
                    "[GL-PERF] tl=%{public}u displayed=%{public}llu fps=%{public}.2f "
                    "upload_bytes=%{public}llu failed_swaps=%{public}llu "
                    "take_us=%{public}llu/%{public}llu/%{public}llu/%{public}llu "
                    "upload_us=%{public}llu/%{public}llu/%{public}llu/%{public}llu "
                    "swap_us=%{public}llu/%{public}llu/%{public}llu/%{public}llu "
                    "total_us=%{public}llu/%{public}llu/%{public}llu/%{public}llu",
                    toplevelId, static_cast<unsigned long long>(displayed), fps,
                    static_cast<unsigned long long>(uploadBytes),
                    static_cast<unsigned long long>(failedSwaps),
                    static_cast<unsigned long long>(Percentile(takeUs, count, 50)),
                    static_cast<unsigned long long>(Percentile(takeUs, count, 95)),
                    static_cast<unsigned long long>(Percentile(takeUs, count, 99)),
                    static_cast<unsigned long long>(*std::max_element(takeUs.begin(), takeUs.end())),
                    static_cast<unsigned long long>(Percentile(uploadUs, count, 50)),
                    static_cast<unsigned long long>(Percentile(uploadUs, count, 95)),
                    static_cast<unsigned long long>(Percentile(uploadUs, count, 99)),
                    static_cast<unsigned long long>(*std::max_element(uploadUs.begin(), uploadUs.end())),
                    static_cast<unsigned long long>(Percentile(swapUs, count, 50)),
                    static_cast<unsigned long long>(Percentile(swapUs, count, 95)),
                    static_cast<unsigned long long>(Percentile(swapUs, count, 99)),
                    static_cast<unsigned long long>(*std::max_element(swapUs.begin(), swapUs.end())),
                    static_cast<unsigned long long>(Percentile(totalUs, count, 50)),
                    static_cast<unsigned long long>(Percentile(totalUs, count, 95)),
                    static_cast<unsigned long long>(Percentile(totalUs, count, 99)),
                    static_cast<unsigned long long>(*std::max_element(totalUs.begin(), totalUs.end())));

        count = 0;
        windowDisplayed = 0;
        uploadBytes = 0;
        failedSwaps = 0;
        startedUs = nowUs;
    }
};

} // namespace

float EglRenderer::globalDisplayScale_ = 1.0f;

void EglRenderer::OnVSync(long long timestamp, void* data)
{
    static_cast<void>(timestamp);
    auto* renderer = static_cast<EglRenderer*>(data);
    {
        std::lock_guard<std::mutex> lock(renderer->vsyncMutex_);
        ++renderer->vsyncSequence_;
    }
    renderer->vsyncCv_.notify_one();
}

void EglRenderer::OnZeroCopyFrameAvailable(void* data)
{
    auto* renderer = static_cast<EglRenderer*>(data);
    if (!renderer) return;
    renderer->zeroCopyFrameSignals_.fetch_add(1, std::memory_order_relaxed);
    renderer->zeroCopyFrameAvailable_.store(true, std::memory_order_release);
}

EGLDisplay EglRenderer::GetSharedDisplay() {
    std::call_once(gDisplayOnce, []() {
        gSharedDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (gSharedDisplay == EGL_NO_DISPLAY) {
            OH_LOG_ERROR(LOG_APP, "[EGL] eglGetDisplay FAILED");
            return;
        }
        EGLint major, minor;
        if (!eglInitialize(gSharedDisplay, &major, &minor)) {
            OH_LOG_ERROR(LOG_APP, "[EGL] eglInitialize FAILED: 0x%{public}x", eglGetError());
            gSharedDisplay = EGL_NO_DISPLAY;
            return;
        }
        OH_LOG_INFO(LOG_APP, "[EGL] shared display init OK EGL %{public}d.%{public}d", major, minor);
    });
    return gSharedDisplay;
}

// -- 全屏 quad 着色器 (Wayland ARGB = BGRA 内存序, 像素着色器中 swizzle) --
static const char* kVS = R"(#version 300 es
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
out vec2 vUV;
void main() { vUV = aUV; gl_Position = vec4(aPos, 0, 1); }
)";

static const char* kFS = R"(#version 300 es
precision mediump float;
in vec2 vUV;
out vec4 oColor;
uniform sampler2D uTex;
uniform float uForceOpaque;
void main() {
    vec4 t = texture(uTex, vUV);
    // uForceOpaque=1: XRGB 帧 (alpha 字节是垃圾, 强制不透明)
    // uForceOpaque=0: ARGB 帧 (layered/shaped 异型窗口, 透传预乘 alpha)
    oColor = vec4(t.bgr, uForceOpaque > 0.5 ? 1.0 : t.a);
}
)";

static const char* kZeroCopyFS = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision mediump float;
in vec2 vUV;
out vec4 oColor;
uniform samplerExternalOES uTex;
uniform mat4 uTransform;
void main() {
    vec4 coord = uTransform * vec4(vUV, 0.0, 1.0);
    oColor = texture(uTex, coord.xy);
}
)";

static GLuint CompileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        OH_LOG_ERROR(LOG_APP, "[EGL] shader compile: %{public}s", log);
    }
    return s;
}

bool EglRenderer::InitZeroCopyConsumer()
{
    if (toplevelId_ == 0 ||
        winehua::GraphicsBroker::GetInstance().GetState().active != winehua::GraphicsBackend::Virgl)
        return false;

    GLuint vertex = CompileShader(GL_VERTEX_SHADER, kVS);
    GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, kZeroCopyFS);
    zeroCopyProgram_ = glCreateProgram();
    glAttachShader(zeroCopyProgram_, vertex);
    glAttachShader(zeroCopyProgram_, fragment);
    glLinkProgram(zeroCopyProgram_);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = GL_FALSE;
    glGetProgramiv(zeroCopyProgram_, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE)
    {
        char log[1024] = {};
        glGetProgramInfoLog(zeroCopyProgram_, sizeof(log), nullptr, log);
        OH_LOG_WARN(LOG_APP, "[VIRGL-ZC][MAIN] external program link failed: %{public}s", log);
        ShutdownZeroCopyConsumer();
        return false;
    }
    zeroCopyTransformLocation_ = glGetUniformLocation(zeroCopyProgram_, "uTransform");
    OH_LOG_INFO(LOG_APP, "[VIRGL-ZC][MAIN] pipeline ready tl=%{public}u", toplevelId_);
    return true;
}

bool EglRenderer::TryAttachZeroCopySurface(uint32_t rendererToplevelId)
{
    if (!zeroCopyProgram_) return false;
    const uint64_t nowUs = PerfNowUs();
    auto& broker = winehua::GraphicsBroker::GetInstance();
    WaylandServer* server = WaylandServer::GetInstance();

    if (zeroCopyRegistered_)
    {
        WaylandServer::ZeroCopyLayerInfo layer;
        if (!server->GetZeroCopyLayerInfo(zeroCopySurfaceKey_, rendererToplevelId, layer))
        {
            ReleaseZeroCopyBinding();
        }
        else
        {
            if (zeroCopyLayerX_ != layer.x || zeroCopyLayerY_ != layer.y ||
                zeroCopyLayerW_ != layer.width || zeroCopyLayerH_ != layer.height)
                zeroCopyGeometryDirty_ = true;
            zeroCopyLayerX_ = layer.x;
            zeroCopyLayerY_ = layer.y;
            zeroCopyLayerW_ = layer.width;
            zeroCopyLayerH_ = layer.height;
            if (zeroCopyFallbackPending_ &&
                layer.shmCommitSerial > zeroCopyFallbackShmSerial_)
            {
                server->SetSurfaceZeroCopy(zeroCopySurfaceKey_, false);
                zeroCopyFallbackPending_ = false;
                zeroCopyHasFrame_ = false;
                OH_LOG_WARN(LOG_APP,
                            "[VIRGL-ZC][MAIN] CPU_FALLBACK tl=%{public}u key=%{public}llu "
                            "shm_serial=%{public}llu baseline=%{public}llu",
                            rendererToplevelId,
                            static_cast<unsigned long long>(zeroCopySurfaceKey_),
                            static_cast<unsigned long long>(layer.shmCommitSerial),
                            static_cast<unsigned long long>(zeroCopyFallbackShmSerial_));
            }
        }
    }

    if (nowUs - zeroCopyLastQueryUs_ < 100000) return zeroCopyRegistered_;
    zeroCopyLastQueryUs_ = nowUs;

    std::vector<winehua::ZeroCopySurfaceInfo> surfaces;
    if (!broker.QueryZeroCopySurfaces(surfaces)) return zeroCopyRegistered_;
    if (zeroCopyRegistered_)
    {
        for (const auto& surface : surfaces)
        {
            if (surface.surfaceKey != zeroCopySurfaceKey_) continue;
            zeroCopySourceW_ = static_cast<int>(surface.width);
            zeroCopySourceH_ = static_cast<int>(surface.height);
            return true;
        }
        return true;
    }

    for (const auto& surface : surfaces)
    {
        if (!surface.surfaceKey || surface.attached) continue;
        WaylandServer::ZeroCopyLayerInfo layer;
        if (!server->GetZeroCopyLayerInfo(surface.surfaceKey, rendererToplevelId, layer)) continue;

        glGenTextures(1, &zeroCopyTexture_);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, zeroCopyTexture_);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        zeroCopyFrameSignals_.store(0, std::memory_order_relaxed);
        zeroCopyFrameAvailable_.store(false, std::memory_order_release);
        zeroCopyImage_ = OH_NativeImage_Create(zeroCopyTexture_, GL_TEXTURE_EXTERNAL_OES);
        if (!zeroCopyImage_)
        {
            ReleaseZeroCopyBinding();
            continue;
        }
        const int32_t sizeResult = OH_ConsumerSurface_SetDefaultSize(
            zeroCopyImage_, static_cast<int32_t>(surface.width),
            static_cast<int32_t>(surface.height));
        const int32_t usageResult = OH_ConsumerSurface_SetDefaultUsage(
            zeroCopyImage_, NATIVEBUFFER_USAGE_HW_RENDER | NATIVEBUFFER_USAGE_HW_TEXTURE);
        const int32_t dropResult = OH_NativeImage_SetDropBufferMode(zeroCopyImage_, true);
        OH_OnFrameAvailableListener listener = {};
        listener.context = this;
        listener.onFrameAvailable = &EglRenderer::OnZeroCopyFrameAvailable;
        if (OH_NativeImage_SetOnFrameAvailableListener(zeroCopyImage_, listener) != 0)
        {
            ReleaseZeroCopyBinding();
            continue;
        }
        zeroCopyListenerSet_ = true;
        zeroCopyProducerWindow_ = OH_NativeImage_AcquireNativeWindow(zeroCopyImage_);
        int32_t queueSize = 0;
        if (zeroCopyProducerWindow_)
            OH_NativeWindow_NativeWindowHandleOpt(
                zeroCopyProducerWindow_, GET_BUFFERQUEUE_SIZE, &queueSize);
        if (!zeroCopyProducerWindow_ ||
            !broker.AttachZeroCopyTarget(
                surface.surfaceKey, zeroCopyProducerWindow_,
                static_cast<uint64_t>(vsyncPeriodNs_.load(std::memory_order_relaxed))))
        {
            ReleaseZeroCopyBinding();
            continue;
        }

        zeroCopySurfaceKey_ = surface.surfaceKey;
        zeroCopyClientPid_ = surface.clientPid;
        zeroCopySurfaceId_ = surface.surfaceId;
        zeroCopySourceW_ = static_cast<int>(surface.width);
        zeroCopySourceH_ = static_cast<int>(surface.height);
        zeroCopyLayerX_ = layer.x;
        zeroCopyLayerY_ = layer.y;
        zeroCopyLayerW_ = layer.width;
        zeroCopyLayerH_ = layer.height;
        zeroCopyRegistered_ = true;
        zeroCopyGeometryDirty_ = true;
        zeroCopyFallbackPending_ = false;
        zeroCopyFallbackShmSerial_ = layer.shmCommitSerial;
        zeroCopyConsecutiveFailures_ = 0;
        zeroCopyLastTimestamp_ = 0;
        zeroCopyTimestampRegressions_ = 0;
        zeroCopyFrames_ = 0;
        zeroCopyFailures_ = 0;
        OH_LOG_INFO(LOG_APP,
                    "[VIRGL-ZC][MAIN] consumer attached tl=%{public}u key=%{public}llu "
                    "pid=%{public}u surface=%{public}u source=%{public}dx%{public}d "
                    "layer=%{public}dx%{public}d+%{public}d,%{public}d queue=%{public}d "
                    "size_ret=%{public}d usage_ret=%{public}d drop_ret=%{public}d",
                    rendererToplevelId,
                    static_cast<unsigned long long>(zeroCopySurfaceKey_),
                    zeroCopyClientPid_, zeroCopySurfaceId_,
                    zeroCopySourceW_, zeroCopySourceH_, zeroCopyLayerW_, zeroCopyLayerH_,
                    zeroCopyLayerX_, zeroCopyLayerY_, queueSize,
                    sizeResult, usageResult, dropResult);
        return true;
    }
    return false;
}

bool EglRenderer::UpdateZeroCopyFrame(int& width, int& height)
{
    if (!zeroCopyRegistered_ || !zeroCopyImage_ ||
        !zeroCopyFrameAvailable_.exchange(false, std::memory_order_acq_rel))
        return false;

    const int32_t updateResult = OH_NativeImage_UpdateSurfaceImage(zeroCopyImage_);
    const int32_t transformResult = updateResult == 0
        ? OH_NativeImage_GetTransformMatrixV2(zeroCopyImage_, zeroCopyTransform_) : -1;
    if (updateResult != 0 || transformResult != 0)
    {
        ++zeroCopyFailures_;
        ++zeroCopyConsecutiveFailures_;
        if (zeroCopyFailures_ == 1 || zeroCopyFailures_ % 60 == 0)
            OH_LOG_WARN(LOG_APP,
                        "[VIRGL-ZC][MAIN] update failed tl=%{public}u update=%{public}d "
                        "transform=%{public}d failures=%{public}llu",
                        toplevelId_, updateResult, transformResult,
                        static_cast<unsigned long long>(zeroCopyFailures_));
        if (zeroCopyReadyPublished_ && !zeroCopyFallbackPending_ &&
            zeroCopyConsecutiveFailures_ >= 8)
        {
            WaylandServer::ZeroCopyLayerInfo layer;
            uint32_t rendererToplevelId = toplevelId_;
            WaylandServer* server = WaylandServer::GetInstance();
            if (server->IsDesktopMode())
                rendererToplevelId = server->GetDesktopRootToplevelId();
            if (server->GetZeroCopyLayerInfo(zeroCopySurfaceKey_, rendererToplevelId, layer))
                zeroCopyFallbackShmSerial_ = layer.shmCommitSerial;
            winehua::GraphicsBroker::GetInstance().SetZeroCopySurfaceReady(
                zeroCopySurfaceKey_, false);
            zeroCopyReadyPublished_ = false;
            zeroCopyFallbackPending_ = true;
            OH_LOG_WARN(LOG_APP,
                        "[VIRGL-ZC][MAIN] fallback pending tl=%{public}u key=%{public}llu "
                        "failures=%{public}u shm_baseline=%{public}llu",
                        rendererToplevelId,
                        static_cast<unsigned long long>(zeroCopySurfaceKey_),
                        zeroCopyConsecutiveFailures_,
                        static_cast<unsigned long long>(zeroCopyFallbackShmSerial_));
        }
        return false;
    }

    zeroCopyConsecutiveFailures_ = 0;
    const int64_t imageTimestamp = OH_NativeImage_GetTimestamp(zeroCopyImage_);
    if (imageTimestamp > 0)
    {
        if (zeroCopyLastTimestamp_ > 0 && imageTimestamp <= zeroCopyLastTimestamp_)
        {
            ++zeroCopyTimestampRegressions_;
            if (zeroCopyTimestampRegressions_ == 1 || zeroCopyTimestampRegressions_ % 60 == 0)
                OH_LOG_WARN(LOG_APP,
                            "[VIRGL-ZC][MAIN] timestamp regression tl=%{public}u "
                            "current=%{public}lld previous=%{public}lld count=%{public}llu",
                            toplevelId_, static_cast<long long>(imageTimestamp),
                            static_cast<long long>(zeroCopyLastTimestamp_),
                            static_cast<unsigned long long>(zeroCopyTimestampRegressions_));
        }
        else
        {
            zeroCopyLastTimestamp_ = imageTimestamp;
        }
    }

    WaylandServer::ZeroCopyLayerInfo layer;
    uint32_t rendererToplevelId = toplevelId_;
    WaylandServer* server = WaylandServer::GetInstance();
    if (server->IsDesktopMode()) rendererToplevelId = server->GetDesktopRootToplevelId();
    if (!server->GetZeroCopyLayerInfo(zeroCopySurfaceKey_, rendererToplevelId, layer))
    {
        ReleaseZeroCopyBinding();
        return false;
    }
    zeroCopyLayerX_ = layer.x;
    zeroCopyLayerY_ = layer.y;
    zeroCopyLayerW_ = layer.width;
    zeroCopyLayerH_ = layer.height;
    width = zeroCopySourceW_;
    height = zeroCopySourceH_;
    zeroCopyHasFrame_ = true;
    if (zeroCopyFallbackPending_)
    {
        zeroCopyFallbackPending_ = false;
        OH_LOG_INFO(LOG_APP,
                    "[VIRGL-ZC][MAIN] fallback cancelled by GPU recovery tl=%{public}u key=%{public}llu",
                    rendererToplevelId,
                    static_cast<unsigned long long>(zeroCopySurfaceKey_));
    }
    if (!zeroCopyReadyPublished_)
    {
        server->SetSurfaceZeroCopy(zeroCopySurfaceKey_, true);
        winehua::GraphicsBroker::GetInstance().SetZeroCopySurfaceReady(
            zeroCopySurfaceKey_, true);
        zeroCopyReadyPublished_ = true;
        OH_LOG_INFO(LOG_APP,
                    "[VIRGL-ZC][MAIN] GPU_ACTIVE tl=%{public}u key=%{public}llu",
                    rendererToplevelId,
                    static_cast<unsigned long long>(zeroCopySurfaceKey_));
    }
    ++zeroCopyFrames_;
    if (zeroCopyFrames_ == 1 || zeroCopyFrames_ % 120 == 0)
        OH_LOG_INFO(LOG_APP,
                    "[VIRGL-ZC][MAIN] frame=%{public}llu tl=%{public}u key=%{public}llu "
                    "source=%{public}dx%{public}d layer=%{public}dx%{public}d+%{public}d,%{public}d "
                    "signals=%{public}llu failures=%{public}llu",
                    static_cast<unsigned long long>(zeroCopyFrames_), toplevelId_,
                    static_cast<unsigned long long>(zeroCopySurfaceKey_), width, height,
                    zeroCopyLayerW_, zeroCopyLayerH_, zeroCopyLayerX_, zeroCopyLayerY_,
                    static_cast<unsigned long long>(zeroCopyFrameSignals_.load()),
                    static_cast<unsigned long long>(zeroCopyFailures_));
    return width > 0 && height > 0;
}

void EglRenderer::ReleaseZeroCopyBinding()
{
    if (zeroCopySurfaceKey_)
    {
        winehua::GraphicsBroker::GetInstance().SetZeroCopySurfaceReady(
            zeroCopySurfaceKey_, false);
        WaylandServer::GetInstance()->SetSurfaceZeroCopy(zeroCopySurfaceKey_, false);
    }
    zeroCopyReadyPublished_ = false;
    zeroCopyFallbackPending_ = false;
    if (zeroCopyRegistered_)
        winehua::GraphicsBroker::GetInstance().DetachZeroCopyTarget(zeroCopySurfaceKey_);
    zeroCopyRegistered_ = false;
    if (zeroCopyImage_ && zeroCopyListenerSet_)
        OH_NativeImage_UnsetOnFrameAvailableListener(zeroCopyImage_);
    zeroCopyListenerSet_ = false;
    zeroCopyProducerWindow_ = nullptr;
    if (zeroCopyImage_) OH_NativeImage_Destroy(&zeroCopyImage_);
    if (zeroCopyTexture_)
    {
        glDeleteTextures(1, &zeroCopyTexture_);
        zeroCopyTexture_ = 0;
    }
    zeroCopyFrameAvailable_.store(false, std::memory_order_release);
    zeroCopyHasFrame_ = false;
    zeroCopyGeometryDirty_ = false;
    zeroCopyConsecutiveFailures_ = 0;
    zeroCopyFallbackShmSerial_ = 0;
    zeroCopyLastTimestamp_ = 0;
    zeroCopyTimestampRegressions_ = 0;
    zeroCopySurfaceKey_ = 0;
    zeroCopyClientPid_ = 0;
    zeroCopySurfaceId_ = 0;
    zeroCopySourceW_ = 0;
    zeroCopySourceH_ = 0;
}

void EglRenderer::ShutdownZeroCopyConsumer()
{
    ReleaseZeroCopyBinding();
    if (zeroCopyProgram_)
    {
        glDeleteProgram(zeroCopyProgram_);
        zeroCopyProgram_ = 0;
    }
}

bool EglRenderer::Init(OHNativeWindow* window, int w, int h) {
    window_ = window;
    width_ = w;
    height_ = h;

    OH_LOG_INFO(LOG_APP, "[EGL] Init tl=%{public}u req=%{public}dx%{public}d", toplevelId_, w, h);

    // 1. 使用共享 EGLDisplay (全进程只 init 一次)
    display_ = GetSharedDisplay();
    if (display_ == EGL_NO_DISPLAY) {
        OH_LOG_ERROR(LOG_APP, "[EGL] shared display unavailable tl=%{public}u", toplevelId_);
        return false;
    }

    EGLConfig cfg;
    EGLint nCfg;
    EGLint attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_NONE
    };
    eglChooseConfig(display_, attrs, &cfg, 1, &nCfg);

    EGLint ctxAttrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    context_ = eglCreateContext(display_, cfg, EGL_NO_CONTEXT, ctxAttrs);

    // 异型窗口 (layered/shaped): 确保 native window buffer 带 alpha 通道,
    // 否则 per-pixel alpha 在 buffer 层就被丢弃 (默认可能是 RGBX)
    OH_NativeWindow_NativeWindowHandleOpt(window_, SET_FORMAT, NATIVEBUFFER_PIXEL_FMT_RGBA_8888);

    // OHOS: EGLNativeWindowType = OHNativeWindow* (cast to unsigned long)
    surface_ = eglCreateWindowSurface(display_, cfg,
                                       reinterpret_cast<EGLNativeWindowType>(window_), nullptr);
    if (surface_ == EGL_NO_SURFACE) {
        OH_LOG_ERROR(LOG_APP, "[EGL] eglCreateWindowSurface failed tl=%{public}u: 0x%{public}x", toplevelId_, eglGetError());
        return false;
    }
    {
        EGLint sw = 0, sh = 0, alphaBits = 0;
        eglQuerySurface(display_, surface_, EGL_WIDTH, &sw);
        eglQuerySurface(display_, surface_, EGL_HEIGHT, &sh);
        eglQuerySurface(display_, surface_, EGL_ALPHA_SIZE, &alphaBits);
        OH_LOG_INFO(LOG_APP, "[EGL] tl=%{public}u eglSurface %{public}dx%{public}d alphaBits=%{public}d",
                    toplevelId_, sw, sh, alphaBits);
    }

    running_ = true;
    thread_ = std::thread(&EglRenderer::RenderLoop, this);
    OH_LOG_INFO(LOG_APP, "[EGL] tl=%{public}u Init done, render thread started OK", toplevelId_);
    return true;
}

void EglRenderer::RenderLoop() {
    if (!eglMakeCurrent(display_, surface_, surface_, context_)) {
        OH_LOG_ERROR(LOG_APP, "[EGL] eglMakeCurrent failed: 0x%{public}x", eglGetError());
        return;
    }

    // NativeVSync owns frame scheduling. Disable EGL's independent swap pacing
    // so a frame does not wait once for VSync and again inside eglSwapBuffers.
    const bool swapIntervalDisabled = eglSwapInterval(display_, 0) == EGL_TRUE;
    OH_LOG_INFO(LOG_APP, "[MW-RNDR] tl=%{public}u eglSwapInterval(0)=%{public}s",
                toplevelId_, swapIntervalDisabled ? "OK" : "FAIL");

    // 2. 着色器
    GLuint vs = CompileShader(GL_VERTEX_SHADER, kVS);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFS);
    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);

    // 3. 全屏 quad VBO
    float quad[] = {
        -1,-1, 0,1,   1,-1, 1,1,   -1, 1, 0,0,
         1,-1, 1,1,   1, 1, 1,0,   -1, 1, 0,0,
    };
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    // 4. 纹理 (初始空)
    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    const bool zeroCopyReady = InitZeroCopyConsumer();
    OH_LOG_INFO(LOG_APP, "[VIRGL-ZC][MAIN] tl=%{public}u path=%{public}s",
                toplevelId_, zeroCopyReady ? "SURFACE_QUEUE" : "SHM_FALLBACK");

    // 5. 渲染循环: 跟随硬件 VSync, 每次只取最新的 toplevel 帧
    FpsCounter fps("render");
    std::vector<uint8_t> px;
    int fw = 0, fh = 0;
    int loopCount = 0;
    bool firstFrameLogged = false;
    bool rendered = false;  // 首帧已渲染后, 无新帧时跳过 GPU 绘制
    RendererPerfWindow perf;

    static constexpr long long kFallbackPeriodNs = 16666667;
    static constexpr auto kVSyncTimeout = std::chrono::milliseconds(100);
    const char vsyncName[] = "WineHuaRenderer";
    OH_NativeVSync* nativeVsync = OH_NativeVSync_Create(vsyncName, sizeof(vsyncName) - 1);
    if (nativeVsync) {
        OH_NativeVSync_ExpectedRateRange expectedRate = {60, 120, 120};
        const int rateResult = OH_NativeVSync_SetExpectedFrameRateRange(
            nativeVsync, &expectedRate);
        OH_LOG_INFO(LOG_APP,
                    "[MW-RNDR] tl=%{public}u request frame rate min=%{public}d "
                    "max=%{public}d expected=%{public}d result=%{public}d",
                    toplevelId_, expectedRate.min, expectedRate.max,
                    expectedRate.expected, rateResult);
    }
    long long vsyncPeriodNs = vsyncPeriodNs_.load(std::memory_order_relaxed);
    long long loggedPeriodNs = 0;
    unsigned int vsyncFailures = 0;
    auto fallbackDeadline = PerfClock::now();

    auto waitForFrameTick = [&]() -> bool {
        if (!running_) return false;

        if (nativeVsync) {
            uint64_t requestedSequence;
            {
                std::lock_guard<std::mutex> lock(vsyncMutex_);
                requestedSequence = vsyncSequence_;
            }

            const int requestResult = OH_NativeVSync_RequestFrame(
                nativeVsync, &EglRenderer::OnVSync, this);
            if (requestResult == 0) {
                std::unique_lock<std::mutex> lock(vsyncMutex_);
                const bool signaled = vsyncCv_.wait_for(lock, kVSyncTimeout, [&]() {
                    return !running_ || vsyncSequence_ != requestedSequence;
                });
                lock.unlock();

                if (!running_) return false;
                if (signaled) {
                    long long period = 0;
                    if (OH_NativeVSync_GetPeriod(nativeVsync, &period) == 0 && period > 0) {
                        vsyncPeriodNs = period;
                        const long long previousPeriod =
                            vsyncPeriodNs_.load(std::memory_order_relaxed);
                        const long long pacingDelta = period > previousPeriod
                            ? period - previousPeriod : previousPeriod - period;
                        if (pacingDelta >= 500000) {
                            vsyncPeriodNs_.store(period, std::memory_order_relaxed);
                            if (zeroCopySurfaceKey_)
                                winehua::GraphicsBroker::GetInstance().SetZeroCopyFramePeriod(
                                    zeroCopySurfaceKey_, static_cast<uint64_t>(period));
                        }
                        const long long periodDelta = period > loggedPeriodNs
                            ? period - loggedPeriodNs : loggedPeriodNs - period;
                        if (loggedPeriodNs == 0 || periodDelta >= 500000) {
                            const double refreshRate = 1000000000.0 / static_cast<double>(period);
                            OH_LOG_INFO(LOG_APP,
                                        "[MW-RNDR] tl=%{public}u NativeVSync period=%{public}lldns "
                                        "rate=%{public}.2fHz",
                                        toplevelId_, period, refreshRate);
                            loggedPeriodNs = period;
                        }
                    }
                    vsyncFailures = 0;
                    fallbackDeadline = PerfClock::now();
                    return true;
                }
            }

            ++vsyncFailures;
            if (vsyncFailures == 1 || vsyncFailures % 120 == 0) {
                OH_LOG_WARN(LOG_APP,
                            "[MW-RNDR] tl=%{public}u NativeVSync unavailable "
                            "request=%{public}d failures=%{public}u, using deadline fallback",
                            toplevelId_, requestResult, vsyncFailures);
            }
        }

        const auto period = std::chrono::nanoseconds(
            vsyncPeriodNs > 0 ? vsyncPeriodNs : kFallbackPeriodNs);
        const auto now = PerfClock::now();
        fallbackDeadline += period;
        if (fallbackDeadline <= now || fallbackDeadline - now > period * 2)
            fallbackDeadline = now + period;
        std::this_thread::sleep_until(fallbackDeadline);
        return running_;
    };

    OH_LOG_INFO(LOG_APP, "[MW-RNDR] tl=%{public}u render loop started pacing=%{public}s",
                toplevelId_, nativeVsync ? "NativeVSync" : "deadline-60Hz");

    while (running_) {
        const uint64_t frameStartedUs = PerfNowUs();
        const uint64_t takeStartedUs = frameStartedUs;
        uint64_t uploadUs = 0;
        bool haveFrame = false;
        bool cpuFrame = false;
        bool zeroCopyFrame = false;
        int zeroCopyWidth = 0;
        int zeroCopyHeight = 0;
        uint32_t useToplevel = toplevelId_;
        WaylandServer* ws = WaylandServer::GetInstance();
        // Desktop mode: root toplevel may be recreated, always use current ID
        if (ws->IsDesktopMode()) useToplevel = ws->GetDesktopRootToplevelId();
        TryAttachZeroCopySurface(useToplevel);
        const bool zeroCopyGeometryFrame = zeroCopyGeometryDirty_;
        zeroCopyGeometryDirty_ = false;
        zeroCopyFrame = UpdateZeroCopyFrame(zeroCopyWidth, zeroCopyHeight);
        if (useToplevel != 0) {
            cpuFrame = ws->TakeToplevelFrame(useToplevel, px, fw, fh);
            if (cpuFrame) {
                // ARGB8888 帧 (layered/shaped 异型窗口) 透传 alpha; XRGB 强制不透明
                frameArgb_ = (ws->GetToplevelShmFormat(useToplevel) == 0);
            }
        } else {
            cpuFrame = ws->TakeFrame(px, fw, fh);
        }
        haveFrame = cpuFrame || zeroCopyFrame || zeroCopyGeometryFrame;
        const uint64_t takeUs = PerfNowUs() - takeStartedUs;

        if (cpuFrame && fw > 0 && fh > 0) {
            const uint64_t uploadStartedUs = PerfNowUs();
            // 存储帧尺寸供输入坐标转换
            frameW_ = fw;
            frameH_ = fh;
            if (!firstFrameLogged) {
                OH_LOG_INFO(LOG_APP, "[MW-RNDR] tl=%{public}u  FIRST FRAME %{public}dx%{public}d px=%{public}zu",
                            useToplevel, fw, fh, px.size());
                firstFrameLogged = true;
            }
            glBindTexture(GL_TEXTURE_2D, texture_);
            int rowLen = (int)px.size() / fh / 4;
            if (rowLen != fw) {
                OH_LOG_WARN(LOG_APP, "[MW-RNDR] UNPACK_ROW_LENGTH rowLen=%{public}d fw=%{public}d px=%{public}zu fh=%{public}d",
                            rowLen, fw, px.size(), fh);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLen);
            }
            // 首帧/尺寸变化: glTexImage2D (分配 GPU 内存)
            // 同尺寸: glTexSubImage2D (复用, 仅 memcpy → GPU)
            if (fw != texW_ || fh != texH_) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fw, fh, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                texW_ = fw; texH_ = fh;
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fw, fh,
                                GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            }
            if (rowLen != fw) {
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            }
            uploadUs = PerfNowUs() - uploadStartedUs;
            rendered = true;
        }
        if (zeroCopyFrame && !firstFrameLogged) {
            OH_LOG_INFO(LOG_APP,
                        "[MW-RNDR] tl=%{public}u FIRST ZERO-COPY FRAME %{public}dx%{public}d",
                        useToplevel, zeroCopyWidth, zeroCopyHeight);
            firstFrameLogged = true;
        }

        // 无新帧且已渲染过首帧 → 跳过 GPU 绘制, 静态桌面节省 GPU 功耗
        if (!haveFrame && rendered) {
            loopCount++;
            if (!waitForFrameTick()) break;
            continue;
        }

        // 获取 EGL surface 实际大小
        EGLint surfW = 0, surfH = 0;
        eglQuerySurface(display_, surface_, EGL_WIDTH, &surfW);
        eglQuerySurface(display_, surface_, EGL_HEIGHT, &surfH);
        if (surfW > 0 && surfH > 0) {
            width_ = surfW;
            height_ = surfH;
        }

        // Letterbox 视口: 保持 Wine 帧宽高比, 居中渲染, 左右或上下黑边
        if (frameW_ > 0 && frameH_ > 0 && width_ > 0 && height_ > 0) {
            float frameAspect = (float)frameW_ / frameH_;
            float surfAspect = (float)width_ / height_;
            if (surfAspect > frameAspect) {
                // Surface 比帧更宽 -> 左右黑边
                vpH_ = height_;
                vpW_ = (int)(height_ * frameAspect);
                vpX_ = (width_ - vpW_) / 2;
                vpY_ = 0;
            } else {
                // Surface 比帧更高 -> 上下黑边 (常见: 手机竖屏)
                vpW_ = width_;
                vpH_ = (int)(width_ / frameAspect);
                vpX_ = 0;
                vpY_ = (height_ - vpH_) / 2;
            }
            glViewport(vpX_, vpY_, vpW_, vpH_);
        } else {
            glViewport(0, 0, width_, height_);
        }

        // 诊断: 前10帧详细打印 surface -> frame -> viewport 完整映射
        if (loopCount < 10) {
            int barTop = vpY_;
            int barBot = height_ - vpY_ - vpH_;
            int barLeft = vpX_;
            int barRight = width_ - vpX_ - vpW_;
            float sA = (float)width_ / height_;
            float fA = frameW_ > 0 && frameH_ > 0 ? (float)frameW_ / frameH_ : 0;
            OH_LOG_INFO(LOG_APP, "[MW-RNDR] diag#%{public}d tl=%{public}u surface=%{public}dx%{public}d(asp=%{public}.2f) frame=%{public}dx%{public}d(asp=%{public}.2f) vp=%{public}dx%{public}d+%{public}d,%{public}d bar=(L%{public}d R%{public}d T%{public}d B%{public}d)",
                        loopCount, useToplevel,
                        width_, height_, sA, frameW_, frameH_, fA,
                        vpW_, vpH_, vpX_, vpY_,
                        barLeft, barRight, barTop, barBot);
        }

        // surface 变化时打印 XComponent → Wine 尺寸映射 (与 ArkTS MW-RESIZE 共用关键字)
        if ((width_ != lastLoggedW_ || height_ != lastLoggedH_) && loopCount >= 10) {
            lastLoggedW_ = width_;
            lastLoggedH_ = height_;
            OH_LOG_INFO(LOG_APP, "[MW-RESIZE] tl=%{public}u surface=%{public}dx%{public}d frame=%{public}dx%{public}d",
                        useToplevel, width_, height_, frameW_, frameH_);
        }
        // ARGB 窗口清透明底 (letterbox 黑边/未覆盖区域也要能透过),
        // 普通窗口清不透明黑底
        if (frameArgb_) glClearColor(0, 0, 0, 0);
        else glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);

        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void*)8);
        glActiveTexture(GL_TEXTURE0);

        if (rendered) {
            glViewport(vpX_, vpY_, vpW_, vpH_);
            glUseProgram(program_);
            glBindTexture(GL_TEXTURE_2D, texture_);
            glUniform1i(glGetUniformLocation(program_, "uTex"), 0);
            glUniform1f(glGetUniformLocation(program_, "uForceOpaque"), frameArgb_ ? 0.0f : 1.0f);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }

        if (zeroCopyHasFrame_ && zeroCopyRegistered_ && frameW_ > 0 && frameH_ > 0 &&
            zeroCopyLayerW_ > 0 && zeroCopyLayerH_ > 0) {
            const int layerViewportX = vpX_ +
                static_cast<int>((static_cast<int64_t>(zeroCopyLayerX_) * vpW_) / frameW_);
            const int layerViewportY = vpY_ +
                static_cast<int>((static_cast<int64_t>(
                    frameH_ - zeroCopyLayerY_ - zeroCopyLayerH_) * vpH_) / frameH_);
            const int layerViewportW = std::max(1, static_cast<int>(
                (static_cast<int64_t>(zeroCopyLayerW_) * vpW_) / frameW_));
            const int layerViewportH = std::max(1, static_cast<int>(
                (static_cast<int64_t>(zeroCopyLayerH_) * vpH_) / frameH_));
            glViewport(layerViewportX, layerViewportY, layerViewportW, layerViewportH);
            glUseProgram(zeroCopyProgram_);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, zeroCopyTexture_);
            glUniform1i(glGetUniformLocation(zeroCopyProgram_, "uTex"), 0);
            glUniformMatrix4fv(zeroCopyTransformLocation_, 1, GL_FALSE, zeroCopyTransform_);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }

        const uint64_t swapStartedUs = PerfNowUs();
        const bool swapOk = eglSwapBuffers(display_, surface_) == EGL_TRUE;
        const uint64_t frameEndedUs = PerfNowUs();
        if (haveFrame) {
            perf.Add(useToplevel, takeUs, uploadUs, frameEndedUs - swapStartedUs,
                     frameEndedUs - frameStartedUs, cpuFrame ? px.size() : 0, swapOk);
        }
        fps.Tick();
        loopCount++;
        if (!waitForFrameTick()) break;
    }

    ShutdownZeroCopyConsumer();
    if (nativeVsync) OH_NativeVSync_Destroy(nativeVsync);
}

void EglRenderer::Shutdown() {
    running_ = false;
    vsyncCv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (display_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surface_ != EGL_NO_SURFACE) {
            eglDestroySurface(display_, surface_);
            surface_ = EGL_NO_SURFACE;
        }
        // 每个 renderer 独立 EGLContext, 各自销毁
        if (context_ != EGL_NO_CONTEXT) {
            eglDestroyContext(display_, context_);
            context_ = EGL_NO_CONTEXT;
        }
        // 不调 eglTerminate: 共享 display 由进程生命周期管理
        // 避免反复 init/terminate 导致 GPU 驱动竞争, 偶发性 SIGSEGV
        OH_LOG_INFO(LOG_APP, "[EGL] tl=%{public}u Shutdown OK (display retained)", toplevelId_);
    }
    // surfaceId 创建的 native window 在这里销毁 (EglRenderer 持有 window_ 指针)
    if (window_) {
        OH_NativeWindow_DestroyNativeWindow(window_);
        window_ = nullptr;
        OH_LOG_INFO(LOG_APP, "[EGL] tl=%{public}u native window destroyed", toplevelId_);
    }
}
