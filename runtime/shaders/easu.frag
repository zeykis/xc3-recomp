#version 450
// AMD FidelityFX Super Resolution 1 (MIT) - EASU, edge-adaptive upscale (the formulation used by Monado's XCDE port,
// MIT). a = (in / out scale xy, offset zw incl. the crop origin), b.xy = input size. 12 taps, Lanczos-2-like weights
// stretched along the local edge, clamped to the nearest 4 (no ringing).
layout(set = 0, binding = 0) uniform sampler2D t;
layout(push_constant) uniform P { vec4 a; vec4 b; vec4 c; } pc;
layout(location = 0) out vec4 o;
vec3 tap(ivec2 p) { return texelFetch(t, clamp(p, ivec2(0), ivec2(pc.b.xy) - 1), 0).rgb; }
float lum(vec3 c) { return c.g + 0.5 * (c.r + c.b); }
void setf(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE) {
    float dx = lD - lB, dy = lE - lA;
    float lx = clamp(abs(dx) / max(max(abs(lD - lC), abs(lC - lB)), 1.0 / 65536.0), 0.0, 1.0);
    float ly = clamp(abs(dy) / max(max(abs(lE - lC), abs(lC - lA)), 1.0 / 65536.0), 0.0, 1.0);
    dir += vec2(dx, dy) * w;
    len += (lx * lx + ly * ly) * w;
}
void tapf(inout vec3 ac, inout float aw, vec2 off, vec2 dir, vec2 len2, float lob, float clp, vec3 c) {
    vec2 v = vec2(dot(off, dir), dot(off, vec2(-dir.y, dir.x))) * len2;
    float d2 = min(dot(v, v), clp);
    float wb = 0.4 * d2 - 1.0, wa = lob * d2 - 1.0;
    wb *= wb; wa *= wa;
    float w = (1.5625 * wb - 0.5625) * wa;
    ac += c * w; aw += w;
}
void main() {
    vec2 pp = floor(gl_FragCoord.xy) * pc.a.xy + pc.a.zw;
    vec2 fpf = floor(pp); pp -= fpf;
    ivec2 fp = ivec2(fpf);
    vec3 bC = tap(fp + ivec2(0, -1)), cC = tap(fp + ivec2(1, -1));
    vec3 eC = tap(fp + ivec2(-1, 0)), fC = tap(fp), gC = tap(fp + ivec2(1, 0)), hC = tap(fp + ivec2(2, 0));
    vec3 iC = tap(fp + ivec2(-1, 1)), jC = tap(fp + ivec2(0, 1)), kC = tap(fp + ivec2(1, 1)), lC = tap(fp + ivec2(2, 1));
    vec3 nC = tap(fp + ivec2(0, 2)), oC = tap(fp + ivec2(1, 2));
    float bL = lum(bC), cL = lum(cC), eL = lum(eC), fL = lum(fC), gL = lum(gC), hL = lum(hC);
    float iL = lum(iC), jL = lum(jC), kL = lum(kC), lL = lum(lC), nL = lum(nC), oL = lum(oC);
    vec2 dir = vec2(0.0);
    float len = 0.0;
    setf(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    setf(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
    setf(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
    setf(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);
    float dr = dot(dir, dir);
    bool zro = dr < 1.0 / 32768.0;
    dir = zro ? vec2(1.0, 0.0) : dir * inversesqrt(dr);
    len = len * 0.5; len *= len;
    float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 - 0.29 * len, clp = 1.0 / lob;
    vec3 mn = min(min(fC, gC), min(jC, kC)), mx = max(max(fC, gC), max(jC, kC));
    vec3 ac = vec3(0.0);
    float aw = 0.0;
    tapf(ac, aw, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, bC);
    tapf(ac, aw, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, cC);
    tapf(ac, aw, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, iC);
    tapf(ac, aw, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, jC);
    tapf(ac, aw, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, fC);
    tapf(ac, aw, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, eC);
    tapf(ac, aw, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, kC);
    tapf(ac, aw, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, lC);
    tapf(ac, aw, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, hC);
    tapf(ac, aw, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, gC);
    tapf(ac, aw, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, oC);
    tapf(ac, aw, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, nC);
    o = vec4(min(mx, max(mn, ac / aw)), 1.0);
}
