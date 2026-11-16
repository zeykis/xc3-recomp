#version 450
// frame generation 4/4: the frame halfway between the previous and the current one. Each pixel follows the motion
// field to where its content is in both frames and blends them; where they disagree (occlusion) the current frame
// wins, where nothing moved (HUD) the current pixel is kept.
// a.xy = output size, a.z = motion field pixel -> output pixel scale, b.xy = motion field size
layout(set = 0, binding = 0) uniform sampler2D cur;
layout(set = 0, binding = 1) uniform sampler2D prv;
layout(set = 0, binding = 2) uniform sampler2D flow;
layout(push_constant) uniform P { vec4 a; vec4 b; vec4 c; } pc;
layout(location = 0) out vec4 o;
vec2 motion(vec2 uv) {   // current -> previous, in output pixels (median of 3x3 against outliers would cost more)
    return textureLod(flow, uv, 0.0).xy * pc.a.z;
}
void main() {
    vec2 sz = pc.a.xy, y = gl_FragCoord.xy, uv = y / sz;
    vec3 cy = texture(cur, uv).rgb, py = texture(prv, uv).rgb;
    if (dot(abs(cy - py), vec3(1.0)) < 0.015) { o = vec4(cy, 1.0); return; }   // unchanged pixel
    vec2 d = motion(uv);
    d = motion((y - 0.5 * d) / sz);
    d = motion((y - 0.5 * d) / sz);
    vec3 c = texture(cur, (y - 0.5 * d) / sz).rgb, p = texture(prv, (y + 0.5 * d) / sz).rgb;
    float diff = dot(abs(c - p), vec3(1.0 / 3.0));
    vec3 r = diff < 0.08 ? 0.5 * (c + p) : c;
    o = vec4(r, 1.0);
}
