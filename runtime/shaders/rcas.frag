#version 450
// AMD FidelityFX Super Resolution 1 (MIT) - RCAS, contrast-adaptive sharpening of the EASU output, drawn into the
// window rectangle c (x, y). a.x = sharpness (exp2(-stops)), b.xy = EASU image size.
layout(set = 0, binding = 0) uniform sampler2D t;
layout(push_constant) uniform P { vec4 a; vec4 b; vec4 c; } pc;
layout(location = 0) out vec4 o;
vec3 tap(ivec2 p) { return texelFetch(t, clamp(p, ivec2(0), ivec2(pc.b.xy) - 1), 0).rgb; }
float lum(vec3 c) { return c.g + 0.5 * (c.r + c.b); }
void main() {
    ivec2 ip = ivec2(gl_FragCoord.xy - pc.c.xy);
    vec3 b = tap(ip + ivec2(0, -1)), d = tap(ip + ivec2(-1, 0)), e = tap(ip), f = tap(ip + ivec2(1, 0)), h = tap(ip + ivec2(0, 1));
    float bL = lum(b), dL = lum(d), eL = lum(e), fL = lum(f), hL = lum(h);
    float nz = 0.25 * (bL + dL + fL + hL) - eL;
    float rng = max(max(max(bL, dL), max(eL, fL)), hL) - min(min(min(bL, dL), min(eL, fL)), hL);
    nz = 1.0 - 0.5 * clamp(abs(nz) / max(rng, 1.0 / 65536.0), 0.0, 1.0);
    vec3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));
    vec3 hitMin = min(mn4, e) / max(4.0 * mx4, vec3(1.0 / 65536.0));
    vec3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, vec3(-1.0 / 65536.0));
    vec3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * pc.a.x * nz;
    o = vec4((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0), 1.0);
}
