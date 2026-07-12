#include "egl_renderer.h"
#include "wayland_server.h"
#include "fps_counter.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <vector>
#include <mutex>
#include <fcntl.h>
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
void main() { oColor = vec4(texture(uTex, vUV).bgr, 1.0); }
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

    // OHOS: EGLNativeWindowType = OHNativeWindow* (cast to unsigned long)
    surface_ = eglCreateWindowSurface(display_, cfg,
                                       reinterpret_cast<EGLNativeWindowType>(window_), nullptr);
    if (surface_ == EGL_NO_SURFACE) {
        OH_LOG_ERROR(LOG_APP, "[EGL] eglCreateWindowSurface failed tl=%{public}u: 0x%{public}x", toplevelId_, eglGetError());
        return false;
    }
    {
        EGLint sw = 0, sh = 0;
        eglQuerySurface(display_, surface_, EGL_WIDTH, &sw);
        eglQuerySurface(display_, surface_, EGL_HEIGHT, &sh);
        OH_LOG_INFO(LOG_APP, "[EGL] tl=%{public}u eglSurface %{public}dx%{public}d", toplevelId_, sw, sh);
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

    // 5. 渲染循环: 60fps, 每帧按 toplevelId 取帧
    FpsCounter fps("render");
    std::vector<uint8_t> px;
    int fw = 0, fh = 0;
    int loopCount = 0;
    bool firstFrameLogged = false;
    bool rendered = false;  // 首帧已渲染后, 无新帧时跳过 GPU 绘制
    RendererPerfWindow perf;
    auto paceFrame = [](uint64_t startedUs) {
        constexpr uint64_t kFramePeriodUs = 16667;
        const uint64_t elapsedUs = PerfNowUs() - startedUs;
        if (elapsedUs < kFramePeriodUs)
            usleep(static_cast<useconds_t>(kFramePeriodUs - elapsedUs));
    };

    OH_LOG_INFO(LOG_APP, "[MW-RNDR] tl=%{public}u render loop started", toplevelId_);

    while (running_) {
        const uint64_t frameStartedUs = PerfNowUs();
        const uint64_t takeStartedUs = frameStartedUs;
        uint64_t uploadUs = 0;
        bool haveFrame = false;
        uint32_t useToplevel = toplevelId_;
        WaylandServer* ws = WaylandServer::GetInstance();
        // Desktop mode: root toplevel may be recreated, always use current ID
        if (ws->IsDesktopMode()) useToplevel = ws->GetDesktopRootToplevelId();
        if (useToplevel != 0) {
            haveFrame = ws->TakeToplevelFrame(useToplevel, px, fw, fh);
        } else {
            haveFrame = ws->TakeFrame(px, fw, fh);
        }
        const uint64_t takeUs = PerfNowUs() - takeStartedUs;

        if (haveFrame && fw > 0 && fh > 0) {
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

        // 无新帧且已渲染过首帧 → 跳过 GPU 绘制, 静态桌面节省 GPU 功耗
        if (!haveFrame && rendered) {
            paceFrame(frameStartedUs);
            loopCount++;
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
        if (fw > 0 && fh > 0 && width_ > 0 && height_ > 0) {
            float frameAspect = (float)fw / fh;
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
            float fA = fw > 0 && fh > 0 ? (float)fw / fh : 0;
            OH_LOG_INFO(LOG_APP, "[MW-RNDR] diag#%{public}d tl=%{public}u surface=%{public}dx%{public}d(asp=%{public}.2f) frame=%{public}dx%{public}d(asp=%{public}.2f) vp=%{public}dx%{public}d+%{public}d,%{public}d bar=(L%{public}d R%{public}d T%{public}d B%{public}d)",
                        loopCount, useToplevel,
                        width_, height_, sA, fw, fh, fA,
                        vpW_, vpH_, vpX_, vpY_,
                        barLeft, barRight, barTop, barBot);
        }

        // surface 变化时打印 XComponent → Wine 尺寸映射 (与 ArkTS MW-RESIZE 共用关键字)
        if ((width_ != lastLoggedW_ || height_ != lastLoggedH_) && loopCount >= 10) {
            lastLoggedW_ = width_;
            lastLoggedH_ = height_;
            OH_LOG_INFO(LOG_APP, "[MW-RESIZE] tl=%{public}u surface=%{public}dx%{public}d frame=%{public}dx%{public}d",
                        useToplevel, width_, height_, fw, fh);
        }
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(program_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void*)8);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture_);
        glUniform1i(glGetUniformLocation(program_, "uTex"), 0);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        const uint64_t swapStartedUs = PerfNowUs();
        const bool swapOk = eglSwapBuffers(display_, surface_) == EGL_TRUE;
        const uint64_t frameEndedUs = PerfNowUs();
        if (haveFrame) {
            perf.Add(useToplevel, takeUs, uploadUs, frameEndedUs - swapStartedUs,
                     frameEndedUs - frameStartedUs, px.size(), swapOk);
        }
        fps.Tick();
        loopCount++;
        paceFrame(frameStartedUs);
    }
}

void EglRenderer::Shutdown() {
    running_ = false;
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
