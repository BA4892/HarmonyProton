#version 450

layout(location = 1) out vec4 out_v1;
layout(location = 2) out vec3 out_v2;
layout(location = 3) out vec3 out_v3;
layout(location = 4) out vec3 out_v4;

void main()
{
    const vec2 positions[3] = vec2[3](
        vec2(-1.0, -1.0),
        vec2( 3.0, -1.0),
        vec2(-1.0,  3.0));
    const vec2 texcoords[3] = vec2[3](
        vec2(0.0, 0.0),
        vec2(2.0, 0.0),
        vec2(0.0, 2.0));
    const vec2 position = positions[gl_VertexIndex];
    const vec2 uv = texcoords[gl_VertexIndex];
    gl_Position = vec4(position, 0.5, 1.0);
    out_v1 = vec4(uv, 0.25, 1.0);
    out_v2 = normalize(vec3(position * 0.125, 1.0));
    out_v3 = normalize(vec3(0.35 + position.x * 0.05,
                            0.55 + position.y * 0.05, 0.75));
    out_v4 = normalize(vec3(0.65 - position.x * 0.04,
                            0.25 - position.y * 0.04, 0.70));
}
