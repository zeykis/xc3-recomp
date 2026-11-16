#version 450
// frame generation 1/4: luma of the current (r) and previous (g) frame at half resolution (bilinear = 2x2 box)
// a.xy = target size
layout(set = 0, binding = 0) uniform sampler2D cur;
layout(set = 0, binding = 1) uniform sampler2D prv;
layout(push_constant) uniform P { vec4 a; vec4 b; vec4 c; } pc;
layout(location = 0) out vec4 o;
float lum(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
void main() {
    vec2 uv = gl_FragCoord.xy / pc.a.xy;
    o = vec4(lum(texture(cur, uv).rgb), lum(texture(prv, uv).rgb), 0.0, 1.0);
}
