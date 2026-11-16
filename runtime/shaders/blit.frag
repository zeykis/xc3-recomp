#version 450
// bilinear copy of the game image's crop rectangle into the window rectangle
// a = crop (x, y, w, h) in source pixels, b.xy = source size, c = window rectangle (x, y, w, h)
layout(set = 0, binding = 0) uniform sampler2D t;
layout(push_constant) uniform P { vec4 a; vec4 b; vec4 c; } pc;
layout(location = 0) out vec4 o;
void main() {
    vec2 f = (gl_FragCoord.xy - pc.c.xy) / pc.c.zw;
    o = vec4(texture(t, (pc.a.xy + f * pc.a.zw) / pc.b.xy).rgb, 1.0);
}
