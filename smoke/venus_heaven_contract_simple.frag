#version 450

layout(set = 0, binding = 5, std140) uniform FragmentConstants {
    vec4 value;
} cb;
layout(set = 0, binding = 10) uniform sampler s0;
layout(set = 0, binding = 16) uniform texture2D t0;
layout(location = 1) in vec4 v1;
layout(location = 0) out vec4 out_color;

void main()
{
    vec4 sampled = texture(sampler2D(t0, s0), fract(v1.xy));
    out_color = mix(sampled, cb.value, 0.0);
}
