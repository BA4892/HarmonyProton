#version 450
layout(set = 0, binding = 0, std140) uniform C0 { vec4 v[4]; } c0;
layout(set = 0, binding = 1, std140) uniform C1 { vec4 v[9]; } c1;
layout(set = 0, binding = 2, std140) uniform C2 { vec4 v[2]; } c2;
layout(set = 0, binding = 3, std140) uniform C3 { vec4 v[4]; } c3;
layout(location = 1) in vec4 v1;
layout(location = 0) out vec4 out_color;
void main() {
  vec4 x = c0.v[0] + c1.v[0] + c2.v[0] + c3.v[0];
  out_color = vec4(fract(x.xyz), 1.0);
}
