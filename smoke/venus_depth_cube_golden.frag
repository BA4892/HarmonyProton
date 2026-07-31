#version 450

#extension GL_EXT_samplerless_texture_functions : require
#extension GL_EXT_texture_shadow_lod : require

layout(set = 0, binding = 1) uniform samplerShadow compareSampler;
layout(set = 0, binding = 2) uniform textureCube sourceDepth;
layout(location = 0) out vec4 outputColor;

vec3 cubeDirection(uint face) {
    if (face == 0) return vec3(1.0, 0.0, 0.0);
    if (face == 1) return vec3(-1.0, 0.0, 0.0);
    if (face == 2) return vec3(0.0, 1.0, 0.0);
    if (face == 3) return vec3(0.0, -1.0, 0.0);
    if (face == 4) return vec3(0.0, 0.0, 1.0);
    return vec3(0.0, 0.0, -1.0);
}

void main() {
    uint face = min(uint(gl_FragCoord.x * 0.09375), 5u);
    float value = textureLod(
        samplerCubeShadow(sourceDepth, compareSampler),
        vec4(cubeDirection(face), 0.5), 0.0);
    outputColor = vec4(value, value, value, 1.0);
}
