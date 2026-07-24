#version 450
layout(set = 0, binding = 16) uniform sampler s0;
layout(set = 0, binding = 32) uniform texture2D t0;
layout(location = 1) in vec4 v1;
layout(location = 0) out vec4 out_color;
void main() { out_color = texture(sampler2D(t0, s0), fract(v1.xy)); }
