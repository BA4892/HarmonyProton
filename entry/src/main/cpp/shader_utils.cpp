#include "shader_utils.h"

#include <hilog/log.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_EGL"

namespace winehua {

const char* kFullscreenQuadVS = R"(#version 300 es
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
out vec2 vUV;
void main() { vUV = aUV; gl_Position = vec4(aPos, 0, 1); }
)";

const char* kFullscreenQuadFS = R"(#version 300 es
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

const char* kZeroCopyExternalFS = R"(#version 300 es
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

GLuint CompileShader(GLenum type, const char* src) {
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

} // namespace winehua
