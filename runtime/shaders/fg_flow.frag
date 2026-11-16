#version 450
// frame generation 3/4: motion of each pixel of this pyramid level, current -> previous frame (block matching on 5x5
// patches, coarse to fine, sub-pixel by a parabola fit). lum: r = current, g = previous. prior: the coarser level's
// motion. a.xy = level size, a.z = 1 if there is a prior, a.w = search radius. out: xy motion (this level's pixels), z cost
layout(set = 0, binding = 0) uniform sampler2D lum;
layout(set = 0, binding = 1) uniform sampler2D prior;
layout(push_constant) uniform P { vec4 a; vec4 b; vec4 c; } pc;
layout(location = 0) out vec4 o;
ivec2 sz, p;
float cur[25];
float cost(ivec2 d) {
    float c = 0.0;
    for (int j = 0; j < 5; j++) for (int i = 0; i < 5; i++)
        c += abs(cur[j * 5 + i] - texelFetch(lum, clamp(p + d + ivec2(i - 2, j - 2), ivec2(0), sz - 1), 0).g);
    return c;
}
void main() {
    sz = ivec2(pc.a.xy); p = ivec2(gl_FragCoord.xy);
    vec2 d0 = pc.a.z > 0.5 ? texture(prior, gl_FragCoord.xy / pc.a.xy).xy * 2.0 : vec2(0.0);
    ivec2 base = ivec2(round(d0));
    int R = int(pc.a.w);
    for (int j = 0; j < 5; j++) for (int i = 0; i < 5; i++) cur[j * 5 + i] = texelFetch(lum, clamp(p + ivec2(i - 2, j - 2), ivec2(0), sz - 1), 0).r;
    float best = 1e9; ivec2 bd = base;
    for (int dy = -R; dy <= R; dy++) for (int dx = -R; dx <= R; dx++) {
        ivec2 d = base + ivec2(dx, dy);
        float c = cost(d) + 0.02 * length(vec2(d) - d0);   // (smoothness: stay near the coarse estimate)
        if (c < best) { best = c; bd = d; }
    }
    float c0 = cost(ivec2(0));   // standing still wins when about as good (HUD, static scenery)
    if (c0 <= best * 1.05 + 0.01) { bd = ivec2(0); best = c0; }
    vec2 f = vec2(bd);
    float cx0 = cost(bd - ivec2(1, 0)), cx1 = cost(bd + ivec2(1, 0)), cy0 = cost(bd - ivec2(0, 1)), cy1 = cost(bd + ivec2(0, 1)), cc = cost(bd);
    float dx = cx0 + cx1 - 2.0 * cc, dy = cy0 + cy1 - 2.0 * cc;
    if (dx > 1e-4) f.x += clamp(0.5 * (cx0 - cx1) / dx, -0.5, 0.5);
    if (dy > 1e-4) f.y += clamp(0.5 * (cy0 - cy1) / dy, -0.5, 0.5);
    o = vec4(f, best, 1.0);
}
