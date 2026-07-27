#pragma once

#include <GLES3/gl3.h>

namespace winehua {

// 全屏 quad 顶点着色器 (Wayland ARGB = BGRA 内存序, 像素着色器中 swizzle)
extern const char* kFullscreenQuadVS;

// 全屏 quad 片段着色器 (SHM 纹理路径)
extern const char* kFullscreenQuadFS;

// 全屏 quad 片段着色器 (zero-copy external OES 纹理路径)
extern const char* kZeroCopyExternalFS;

GLuint CompileShader(GLenum type, const char* src);

} // namespace winehua
