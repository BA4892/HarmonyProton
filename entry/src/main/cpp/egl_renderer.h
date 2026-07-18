#pragma once
#include <ace/xcomponent/native_interface_xcomponent.h>
#include <native_window/external_window.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <thread>
#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <mutex>

struct OH_NativeImage;

// 最小 EGL 渲染器: 从 WaylandServer 取帧 -> GL 纹理 -> XComponent 上屏
// 所有实例共享同一个 EGLDisplay (避免反复 init/terminate 导致 GPU 驱动竞争)
// 每个实例拥有独立的 EGLContext + EGLSurface
class EglRenderer {
public:
    // 获取/初始化共享的 EGLDisplay (首次调用时初始化, 线程安全)
    static EGLDisplay GetSharedDisplay();

    bool Init(OHNativeWindow* window, int w, int h);
    void Shutdown();

    uint32_t GetToplevelId() const { return toplevelId_; }
    void SetToplevelId(uint32_t id) { toplevelId_ = id; }
    void SetSize(int w, int h) { width_ = w; height_ = h; }
    bool IsValid() const { return running_; }

    // 尺寸 getters (供输入坐标转换: 触控坐标 -> wine 内容坐标)
    int GetWidth() const { return width_; }
    int GetHeight() const { return height_; }
    int GetFrameWidth() const { return frameW_; }
    int GetFrameHeight() const { return frameH_; }
    // Letterbox viewport (保持 Wine 帧宽高比居中渲染的视口)
    int GetVpX() const { return vpX_; }
    int GetVpY() const { return vpY_; }
    int GetVpW() const { return vpW_; }
    int GetVpH() const { return vpH_; }

    static void SetGlobalDisplayScale(float s) { globalDisplayScale_ = s; }
    static float GetGlobalDisplayScale() { return globalDisplayScale_; }

private:
    void RenderLoop();
    static void OnVSync(long long timestamp, void* data);
    static void OnZeroCopyFrameAvailable(void* data);
    bool InitZeroCopyConsumer();
    bool TryAttachZeroCopySurface(uint32_t rendererToplevelId);
    bool UpdateZeroCopyFrame(int& width, int& height);
    void ReleaseZeroCopyBinding();
    void ShutdownZeroCopyConsumer();

    OHNativeWindow* window_ = nullptr;
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;

    GLuint texture_ = 0;
    GLuint program_ = 0;
    GLuint vbo_ = 0;
    OH_NativeImage* zeroCopyImage_ = nullptr;
    OHNativeWindow* zeroCopyProducerWindow_ = nullptr;
    GLuint zeroCopyTexture_ = 0;
    GLuint zeroCopyProgram_ = 0;
    GLint zeroCopyTransformLocation_ = -1;
    std::atomic<bool> zeroCopyFrameAvailable_{false};
    std::atomic<uint64_t> zeroCopyFrameSignals_{0};
    uint64_t zeroCopyFrames_ = 0;
    uint64_t zeroCopyFailures_ = 0;
    uint64_t zeroCopyFallbackShmSerial_ = 0;
    uint64_t zeroCopyTimestampRegressions_ = 0;
    int64_t zeroCopyLastTimestamp_ = 0;
    uint64_t zeroCopySurfaceKey_ = 0;
    uint64_t zeroCopyLastQueryUs_ = 0;
    uint32_t zeroCopyClientPid_ = 0;
    uint32_t zeroCopySurfaceId_ = 0;
    int zeroCopySourceW_ = 0;
    int zeroCopySourceH_ = 0;
    int zeroCopyLayerX_ = 0;
    int zeroCopyLayerY_ = 0;
    int zeroCopyLayerW_ = 0;
    int zeroCopyLayerH_ = 0;
    bool zeroCopyRegistered_ = false;
    bool zeroCopyListenerSet_ = false;
    bool zeroCopyReadyPublished_ = false;
    bool zeroCopyHasFrame_ = false;
    bool zeroCopyFallbackPending_ = false;
    bool zeroCopyGeometryDirty_ = false;
    uint32_t zeroCopyConsecutiveFailures_ = 0;
    float zeroCopyTransform_[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
    };

    int width_ = 0, height_ = 0;
    int frameW_ = 0, frameH_ = 0;  // Wine 帧内容尺寸 (坐标转换)
    bool frameArgb_ = false;       // 当前帧是 ARGB8888 (layered/shaped 异型窗口, 透传 alpha)
    int texW_ = 0, texH_ = 0;      // 上次上传的纹理尺寸 (用于避免每帧 glTexImage2D)
    int vpX_ = 0, vpY_ = 0, vpW_ = 0, vpH_ = 0;  // Letterbox 视口 (保持宽高比)
    int bufW_ = 0, bufH_ = 0;  // 上次 SET_BUFFER_GEOMETRY 的值, 避免重复调用
    int lastLoggedW_ = 0, lastLoggedH_ = 0;  // 上次输出 resize 日志时的 surface 尺寸
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::mutex vsyncMutex_;
    std::condition_variable vsyncCv_;
    uint64_t vsyncSequence_ = 0;
    std::atomic<long long> vsyncPeriodNs_{16666667};

    uint32_t toplevelId_ = 0;

    static float globalDisplayScale_;
};
