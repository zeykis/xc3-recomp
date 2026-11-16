// Headless NVN: the game's GPU API, implemented as a functional object model without rendering.
//
// Every NVN object (memory pools, buffers, textures, programs, queues, windows...) is tracked by
// the address of the guest struct the game hands us. "GPU addresses" are simply host pointers, so
// buffers and pools are directly CPU-visible. Draw/state commands are accepted and dropped.
// This lets the game's renderer run its real logic end to end; the Vulkan backend (milestone M3)
// replaces the command handlers below and adds actual rendering + presentation.
#include <atomic>
#include <cstring>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <set>
#include <string>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>
#include "gpu.h"
#include "nvn_format.h"
#include "gpu_shader.h"
#include "settings.h"
#include <algorithm>
#include <map>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif
extern "C" {
#include "xrt_internal.h"
}
#define P(i) ((void *)(uintptr_t)c->x[i])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define HLE(name) static void name(Ctx *c)

static std::mutex g_nl;

// ---------------------------------------------------------------- builders (generic field bag)
struct Builder {
    uint64_t storage = 0, size = 0, pool = 0, offset = 0, packaged = 0;
    int flags = 0, target = 1, format = 0, width = 1, height = 1, depth = 1, levels = 1, samples = 0;
    int num_textures = 0; uint64_t textures = 0;
    int ds_mode = 0;
    uint8_t swz[4] = {2, 3, 4, 5};   // NVNtextureSwizzle per channel: 0 ZERO, 1 ONE, 2 R, 3 G, 4 B, 5 A          // NVNtextureDepthStencilMode: 0 depth, 1 stencil (sampling a depth-stencil texture)
};
static std::unordered_map<uint64_t, Builder> g_builders;
static Builder &B(uint64_t a) { return g_builders[a]; }

struct Pool { uint64_t storage, size; int flags; };
struct Buf { uint64_t addr, size; };
struct Tex { Builder b; uint64_t addr, size; GpuTex *gpu = nullptr; bool gpu_tried = false; bool alias = false; bool dirty = false;
             bool is_rt = false, shared = false; uint64_t mem_hash = 0, hash_frame = ~0ULL; uint32_t mem_bytes = 0; };   // alias: gpu owned by another Tex on the same memory
struct Win { int num = 0, next = 0; std::vector<uint64_t> textures; };   // texture pointers copied at init
static std::unordered_map<uint64_t, Pool> g_pools;
static std::unordered_map<uint64_t, Buf> g_bufs;
static std::unordered_map<uint64_t, Tex> g_texs;
static std::unordered_map<uint64_t, Win> g_wins;
static std::atomic<uint64_t> g_frames{0}, g_cmd_handles{1};

// Locks (milestone M5, render worker). The NVN calls run on the game's threads; the NVN -> Vulkan translation of what
// they submit runs on the render worker (below), in parallel with the game recording its next frame.
//  * object lock (g_nl, NLOCK): everything the translation reads - textures, pools, texture / sampler pools,
//    programs - and the Vulkan backend. Held by the worker while it runs a job; it lets a waiting game thread in
//    between two commands (g_nl_waiters), so a texture created mid-frame never waits for a whole frame's translation.
//  * recording lock (g_rl, RLOCK): command lists being recorded, submitted handles, builders, state objects,
//    views. Never taken by the worker, so recording commands never waits on translation.
// Game-side changes to what the worker reads take both (BOTH, object lock first); game-side reads of those tables
// take the recording lock only (they're only ever changed under both).
static std::mutex g_rl;
static std::atomic<int> g_nl_waiters{0};
static std::atomic<uint64_t> g_lock_blocked, g_lock_wait_ns, g_lock_total;
struct NvnLock {
    NvnLock() {
        g_lock_total.fetch_add(1, std::memory_order_relaxed);
        if (g_nl.try_lock()) return;
        uint64_t t0 = plat_ticks_ns(); g_nl_waiters++; g_nl.lock(); g_nl_waiters--;
        g_lock_blocked.fetch_add(1, std::memory_order_relaxed); g_lock_wait_ns.fetch_add(plat_ticks_ns() - t0, std::memory_order_relaxed);
    }
    ~NvnLock() { g_nl.unlock(); }
};
#define NLOCK NvnLock _l
#define RLOCK std::lock_guard<std::mutex> _rl(g_rl)
#define BOTH NvnLock _l; std::lock_guard<std::mutex> _rl(g_rl)
// worker side, between two commands: let game threads waiting for the object lock in
static std::atomic<uint64_t> g_nl_yield_ns;   // worker time given to game threads at nl_yield
uint64_t gpu_nl_wait_ns(bool reset) { return reset ? g_nl_yield_ns.exchange(0) : g_nl_yield_ns.load(); }
static void nl_yield() {
    if (!g_nl_waiters.load(std::memory_order_relaxed)) return;
    uint64_t t0 = plat_ticks_ns();
    g_nl.unlock();
    for (int i = 0; i < 200 && g_nl_waiters.load(std::memory_order_relaxed); i++) std::this_thread::yield();
    g_nl.lock();
    g_nl_yield_ns += plat_ticks_ns() - t0;
}

HLE(ok_true) { RET(1); }
HLE(ret0) { RET(0); }
HLE(nop) { }

// setters: store into the builder bag
HLE(b_defaults) { RLOCK; g_builders[c->x[0]] = Builder{}; }
HLE(b_flags) { RLOCK; B(c->x[0]).flags = (int)c->x[1]; }
HLE(mpb_storage) { RLOCK; Builder &b = B(c->x[0]); b.storage = c->x[1]; b.size = c->x[2]; }
HLE(bb_storage) { RLOCK; Builder &b = B(c->x[0]); b.pool = c->x[1]; b.offset = c->x[2]; b.size = c->x[3]; }
HLE(tb_target) { RLOCK; B(c->x[0]).target = (int)c->x[1]; }
HLE(tb_format) { RLOCK; B(c->x[0]).format = (int)c->x[1]; }
HLE(tb_swizzle) { RLOCK; Builder &b = B(c->x[0]); for (int i = 0; i < 4; i++) b.swz[i] = (uint8_t)c->x[1 + i]; }
HLE(tb_size1d) { RLOCK; Builder &b = B(c->x[0]); b.width = (int)c->x[1]; b.height = 1; b.depth = 1; }
HLE(tb_size2d) {
    {   // XRT_WATCH_SIZE=WxH: log who creates textures of that size (call chain from the guest frame records)
        static int ww = -1, wh = -1, n; if (ww < 0) { const char *e = getenv("XRT_WATCH_SIZE"); ww = 0; if (e) sscanf(e, "%dx%d", &ww, &wh); }
        if (ww > 0 && (int)c->x[1] == ww && (int)c->x[2] == wh && n++ < 12) {
            char b[256]; int k = snprintf(b, sizeof b, "lr main+%llx", (unsigned long long)(c->x[30] - (uint64_t)(uintptr_t)g_img.base));
            uint64_t fp = c->x[29];
            for (int i = 0; i < 6 && fp && k < 220; i++) { uint64_t lr = LD64(fp + 8); k += snprintf(b + k, sizeof b - k, " <- main+%llx", (unsigned long long)(lr - (uint64_t)(uintptr_t)g_img.base)); fp = LD64(fp); }
            LOG(0, "[nvn] texture %dx%d: %s", ww, wh, b);
        }
    }
    RLOCK; Builder &b = B(c->x[0]); b.width = (int)c->x[1]; b.height = (int)c->x[2]; b.depth = 1; }
HLE(tb_size3d) { RLOCK; Builder &b = B(c->x[0]); b.width = (int)c->x[1]; b.height = (int)c->x[2]; b.depth = (int)c->x[3]; }
HLE(tb_depth) { RLOCK; B(c->x[0]).depth = (int)c->x[1]; }
HLE(tb_levels) { RLOCK; B(c->x[0]).levels = (int)c->x[1]; }
HLE(tb_samples) { RLOCK; B(c->x[0]).samples = (int)c->x[1]; }
HLE(tb_storage) { RLOCK; Builder &b = B(c->x[0]); b.pool = c->x[1]; b.offset = c->x[2]; }
HLE(tb_packaged) { RLOCK; B(c->x[0]).packaged = c->x[1]; }
HLE(tb_ds_mode) { RLOCK; B(c->x[0]).ds_mode = (int)c->x[1]; }
HLE(wb_textures) { RLOCK; Builder &b = B(c->x[0]); b.num_textures = (int)c->x[1]; b.textures = c->x[2]; }

// bytes per texel by NVN format (coarse; exact table comes with the Vulkan backend)
static double bpp(int fmt) {
    if (fmt >= 0x34 && fmt <= 0x3f) return 4;      // depth formats
    if (fmt >= 0x40 && fmt <= 0x6f) return 1;      // BCn / ASTC-ish: ~1 byte/texel upper bound
    if (fmt >= 0x2c && fmt <= 0x33) return 16;     // RGBA32
    if (fmt >= 0x26 && fmt <= 0x2b) return 8;      // RGBA16
    return 4;
}
static uint64_t tex_size(const Builder &b) {
    int d = b.depth ? b.depth : 1;
    if (b.target == 8 && d < 6) d = 6;           // cube maps: 6 faces even when only a 2D size was given
    double s = (double)b.width * b.height * d * bpp(b.format);
    if (b.levels > 1) s *= 1.34;
    if (b.samples > 1) s *= b.samples;
    uint64_t r = (uint64_t)s;
    return (r + 0xffff) & ~0xffffULL;            // block-linear surfaces are page-granular
}
HLE(tb_storage_size) { RLOCK; RET(tex_size(B(c->x[0]))); }
HLE(tb_storage_align) { RET(0x10000); }

// initialize from builder
static uint64_t pool_ptr(uint64_t pool) { auto it = g_pools.find(pool); return it == g_pools.end() ? 0 : it->second.storage; }
// Internal render resolution (M5). XC3 2.2.0 renders its 3D scene at 960x544 and reconstructs it (temporal upscale)
// into 1920x1088; the image it presents stays 1920x1080. The renderer creates its targets from a settings struct
// (main+0xf5905c: output size at +0x18, scene size at +0x34) and a few other targets from read-only constants
// (main+0x14e602c: the same two sizes). XRT_RES_SCALE=s multiplies the scene size (default 1; 2 = 1920x1088, 2.66 ~
// 1440p, 4 = 4K-class), the reconstruction target growing with it when the scene would be larger. The struct is
// adjusted by an entry hook (recomp/hooks.txt) each time the renderer (re)creates its targets.
static int g_res_w, g_res_h;                    // XRT_RES_SCALE scene size (0 = native 960x544)
static void res_fix(int32_t *scene, int32_t *recon) {
    if (scene[0] == 960 && scene[1] == 544) { scene[0] = g_res_w; scene[1] = g_res_h; }
    if (recon[0] == 1920 && recon[1] == 1088 && (g_res_w > 1920 || g_res_h > 1088)) { recon[0] = std::max(1920, g_res_w); recon[1] = std::max(1088, g_res_h); }
}
static void res_hook(Ctx *c) {   // main+0xf5905c(renderer): targets are about to be (re)created
    uint64_t obj = c->x[0], p = obj ? LD64(obj + 8) : 0, st = p ? LD64(p + 0xa90) : 0;
    if (!st) return;
    int32_t *out = (int32_t *)(uintptr_t)(st + 0x18), *scene = (int32_t *)(uintptr_t)(st + 0x34);
    static int n; if (n++ < 4) LOG(0, "[res] render setup: output %dx%d, scene %dx%d", out[0], out[1], scene[0], scene[1]);
    if (scene[0] == 960 && scene[1] == 544) { scene[0] = g_res_w; scene[1] = g_res_h; }
}
extern "C" void xrt_apply_render_scale(void) {   // (main.cpp, at boot)
    double sc = settings().res_scale;   // (xc3.ini resolution_scale / XRT_RES_SCALE; needs a restart: targets are made once)
    if (!(sc > 0) || sc == 1.0) return;
    int32_t *v = (int32_t *)(g_img.base + 0x14e602c);
    if (v[0] != 960 || v[1] != 544 || v[2] != 1920 || v[3] != 1088) { LOG(0, "[res] render size constants not found (%d %d %d %d): XRT_RES_SCALE ignored", v[0], v[1], v[2], v[3]); return; }
    if (sc > 4) sc = 4;
    g_res_w = ((int)(960 * sc + 8) / 16) * 16; g_res_h = ((int)(544 * sc + 8) / 16) * 16;
    res_fix(v, v + 2);
    xrt_register_hook(0xf5905c, res_hook);
    LOG(0, "[res] scene %dx%d (x%.2f), reconstruction %dx%d", g_res_w, g_res_h, sc, v[2], v[3]);
}
HLE(mp_init) { BOTH; Builder &b = B(c->x[1]); g_pools[c->x[0]] = Pool{b.storage, b.size, b.flags}; gpu_pool_register(b.storage, b.size); RET(1); }
HLE(mp_map) { RLOCK; RET(pool_ptr(c->x[0])); }
HLE(mp_addr) { RLOCK; RET(pool_ptr(c->x[0])); }
HLE(mp_size) { RLOCK; auto it = g_pools.find(c->x[0]); RET(it == g_pools.end() ? 0 : it->second.size); }
HLE(mp_flags) { RLOCK; auto it = g_pools.find(c->x[0]); RET(it == g_pools.end() ? 0 : it->second.flags); }
HLE(buf_init) { RLOCK; Builder &b = B(c->x[1]); g_bufs[c->x[0]] = Buf{pool_ptr(b.pool) + b.offset, b.size}; RET(1); }
HLE(buf_map) { RLOCK; RET(g_bufs[c->x[0]].addr); }
HLE(buf_addr) { RLOCK; RET(g_bufs[c->x[0]].addr); }
HLE(buf_size) { RLOCK; RET(g_bufs[c->x[0]].size); }
// Release a texture's GPU image (aliases on the same memory lose their pointer and re-resolve later)
// Bumped whenever a texture ID could resolve differently (pools, textures, views, CPU texel writes): the draw
// path memoizes ID -> (GPU texture, flags, swizzle) per frame and generation (gpu_vk_draw.inc).
static std::atomic<uint64_t> g_tex_gen{1};
uint64_t nvn_tex_generation() { return g_tex_gen.load(std::memory_order_relaxed); }
static void release_gpu(Tex &t) {
    g_tex_gen++;
    if (!t.gpu) return;
    if (!t.alias) {
        if (t.shared)   // (only textures other NVN textures alias: the scan is over every texture)
            for (auto &kv : g_texs) if (&kv.second != &t && kv.second.gpu == t.gpu) { kv.second.gpu = nullptr; kv.second.gpu_tried = false; kv.second.alias = false; }
        gpu_tex_destroy(t.gpu);
    }
    t.gpu = nullptr; t.alias = false; t.shared = false;
}
HLE(tex_init) {
    BOTH; g_tex_gen++; Builder &b = B(c->x[1]);
    auto it = g_texs.find(c->x[0]);
    if (it != g_texs.end()) release_gpu(it->second);
    Tex nt{b, pool_ptr(b.pool) + b.offset, tex_size(b)};
    if (nt.b.target == 8 && nt.b.depth < 6) nt.b.depth = 6;   // NVN cube maps always have 6 faces (SetSize2D leaves depth at 1)
    g_texs[c->x[0]] = nt;
    RET(1);
}
HLE(tex_finalize) {
    BOTH; auto it = g_texs.find(c->x[0]);
    if (it == g_texs.end()) return;
    release_gpu(it->second);
    g_texs.erase(it);
}
static const Tex &tex_ro(uint64_t a) { static const Tex none{}; auto it = g_texs.find(a); return it == g_texs.end() ? none : it->second; }   // (no insertion: the worker reads g_texs)
HLE(tex_target) { RLOCK; RET(tex_ro(c->x[0]).b.target); }
HLE(tex_format) { RLOCK; RET(tex_ro(c->x[0]).b.format); }
HLE(tex_flags) { RLOCK; RET(tex_ro(c->x[0]).b.flags); }
HLE(tex_width) { RLOCK; RET(tex_ro(c->x[0]).b.width); }
HLE(tex_height) { RLOCK; RET(tex_ro(c->x[0]).b.height); }
HLE(tex_depth) { RLOCK; RET(tex_ro(c->x[0]).b.depth); }
HLE(tex_levels) { RLOCK; RET(tex_ro(c->x[0]).b.levels); }
HLE(tex_storage_size) { RLOCK; RET(tex_ro(c->x[0]).size); }
HLE(win_init) {
    RLOCK; Builder &b = B(c->x[1]); Win w; w.num = b.num_textures;
    for (int i = 0; i < b.num_textures && b.textures; i++) w.textures.push_back(LD64(b.textures + 8 * i));
    g_wins[c->x[0]] = w; RET(1);
}
HLE(win_acquire) {   // (window, sync, int* index) -> NVNwindowAcquireTextureResult (0 = success)
    RLOCK; Win &w = g_wins[c->x[0]];
    int n = w.num > 0 ? w.num : 2;
    *(int32_t *)P(2) = w.next; w.next = (w.next + 1) % n;
    RET(0);
}

// ---------------------------------------------------------------- device queries
static int device_integer(int pname) {
    switch (pname) {
    case 0: return 53;        // API_MAJOR_VERSION
    case 1: return 400;       // API_MINOR_VERSION (>= anything the game was built against)
    case 2: return 14;  case 3: return 65536; case 4: return 256; case 5: return 8; case 6: return 16;
    case 7: return 4;   case 8: return 16;    case 9: return 32;  case 10: return 16; case 11: return 4;
    case 12: return 4;  case 13: return 4;    case 14: return 16; case 15: return 32; case 16: return 32;
    case 17: return 256; case 18: return 256; case 19: return 4;  case 20: return 8;  case 21: return 512;
    case 22: return 256; case 23: return 256; case 24: return 1024; case 25: return 4096; case 26: return 131072;
    case 27: return 16; case 28: return 1536; case 29: return 1024; case 30: return 64; case 31: return 1536;
    case 32: case 33: case 34: return 65535; case 35: return 8; case 36: return 1048576; case 37: return 4096;
    case 38: return 16;
    default: return 256;
    }
}
HLE(dev_get_integer) {   // (device, NVNdeviceInfo pname, int* value)
    int pname = (int)c->x[1], v = device_integer(pname);
    { RLOCK; static std::set<int> seen; if (seen.insert(pname).second) LOG(2, "[nvn] DeviceGetInteger(%d) -> %d", pname, v); }
    *(int32_t *)P(2) = v;
}
HLE(dev_timestamp) { RET(plat_ticks_ns()); }
// NVNwindowOriginMode: the game sets 1, which renders upside down without a flip, i.e. lower-left.
// XRT_FLIP_Y=0/1 overrides.
static int g_origin_mode = 0;
HLE(dev_origin_mode) { g_origin_mode = (int)c->x[1]; }
// NVNdepthMode: 0 = NEAR_IS_MINUS_W (OpenGL clip space, needs z' = (z + w) / 2), 1 = NEAR_IS_ZERO (same as Vulkan)
HLE(dev_depth_mode) { BOTH; LOG(1, "[nvn] depth mode %d", (int)c->x[1]); gpu_set_depth_mode((int)c->x[1]); }
HLE(dev_tex_handle) { RET(((uint64_t)(uint32_t)c->x[2] << 20) | (uint32_t)c->x[1]); }
HLE(dev_image_handle) { RET((uint32_t)c->x[1]); }
HLE(dev_sep_handle) { RET((uint32_t)c->x[1]); }

// ---------------------------------------------------------------- queues / sync / present
static uint64_t g_xfb_watch, g_xfb_watch_size; static bool g_xfb_active;   // last transform-feedback buffer bound (diagnostics)
static uint64_t g_ts_base, g_ts_count;          // GPU timestamp model (ReportCounter)
static uint64_t g_vs_control;                   // control data of the stage feeding transform feedback (diagnostics)
struct XfbLayout { uint8_t locs[128] = {}; int count = 0, stride = 0; uint64_t control = 0; };
static XfbLayout g_xfb_layout[GS_COUNT];
static int g_render = -1;              // XRT_NO_DRAW=1 disables draws (clears/present only)
// nvnSyncWait / nvnQueueFinish: the game is about to reuse memory the GPU reads, so run what was recorded
// (XRT_NO_SYNC_FLUSH=1: old behaviour, report signaled without running anything)
static bool sync_flush_enabled() { static int e = -1; if (e < 0) e = getenv("XRT_NO_SYNC_FLUSH") ? 0 : 1; return e == 1; }
// XRT_ORDER=frame: log the order of submits / fences / waits / presents for 3 frames from that one
static void order_log(const char *what, Ctx *c) {
    static long from = -2; if (from == -2) { const char *e = getenv("XRT_ORDER"); from = e ? atol(e) : -1; }
    long f = (long)g_frames.load();
    if (from < 0 || f < from || f >= from + 3) return;
    LOG(0, "[order] frame %ld %8.3f ms %s (x1 %llx) thread %p", f, (plat_ticks_ns() % 1000000000ull) / 1e6, what, (unsigned long long)c->x[1], (void *)c);
}
static std::atomic<uint64_t> g_n_sync_wait, g_n_queue_fence, g_n_cmd_fence, g_n_cmd_wait, g_n_finish;
static std::atomic<uint64_t> g_worker_wait_ns, g_worker_waits;   // game threads blocked on the render worker
HLE(cmd_fence_sync) { g_n_cmd_fence++; { static int n; if (n++ < 6) LOG(1, "[nvn] CommandBufferFenceSync(%llx, sync %llx) lr %llx", (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[30]); } }
HLE(cmd_wait_sync) { g_n_cmd_wait++; { static int n; if (n++ < 6) LOG(1, "[nvn] CommandBufferWaitSync(%llx, sync %llx) lr %llx", (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[30]); } }
void nvn_sync_stats(double frames) {
    LOG(1, "[prof] nvn lock per frame: %.0f acquisitions, %.0f blocked, %.2f ms waiting (all threads)", g_lock_total.exchange(0) / frames,
        g_lock_blocked.exchange(0) / frames, g_lock_wait_ns.exchange(0) / 1e6 / frames);
    LOG(1, "[prof] nvn sync per frame: %.2f SyncWait, %.2f QueueFenceSync, %.2f CommandBufferFenceSync, %.2f CommandBufferWaitSync, %.2f QueueFinish",
        g_n_sync_wait.exchange(0) / frames, g_n_queue_fence.exchange(0) / frames, g_n_cmd_fence.exchange(0) / frames, g_n_cmd_wait.exchange(0) / frames, g_n_finish.exchange(0) / frames);
    LOG(1, "[prof] render worker: game blocked %.2f ms/frame in %.1f waits/frame", g_worker_wait_ns.exchange(0) / 1e6 / frames, g_worker_waits.exchange(0) / frames);
}
// ---------------------------------------------------------------- texture views
// NVNtextureView: the game embeds views in its own structs with other data right after them, so we
// must not write into the guest struct. Fields are kept in a side table keyed by the view address.
struct View { uint32_t magic, set; int32_t base_level, num_levels, base_layer, num_layers, format, target; uint8_t swz[4]; };
static const uint32_t VIEW_MAGIC = 1;
enum { VS_LEVELS = 1, VS_LAYERS = 2, VS_FORMAT = 4, VS_TARGET = 8, VS_SWIZZLE = 16 };
static std::unordered_map<uint64_t, View> g_views;
static View *view_of(uint64_t a) { View &v = g_views[a]; v.magic = VIEW_MAGIC; return &v; }
static const View *find_view(uint64_t a) { auto it = g_views.find(a); return it == g_views.end() ? nullptr : &it->second; }
HLE(tv_defaults) { RLOCK; g_views[c->x[0]] = View{VIEW_MAGIC, 0, 0, 0, 0, 0, 0, 0, {2, 3, 4, 5}}; }
HLE(tv_swizzle) { RLOCK; View *v = view_of(c->x[0]); v->set |= VS_SWIZZLE; for (int i = 0; i < 4; i++) v->swz[i] = (uint8_t)c->x[1 + i]; }
HLE(tv_levels) { RLOCK; View *v = view_of(c->x[0]); v->set |= VS_LEVELS; v->base_level = (int)c->x[1]; v->num_levels = (int)c->x[2]; }
HLE(tv_layers) { RLOCK; View *v = view_of(c->x[0]); v->set |= VS_LAYERS; v->base_layer = (int)c->x[1]; v->num_layers = (int)c->x[2]; }
HLE(tv_format) { RLOCK; View *v = view_of(c->x[0]); v->set |= VS_FORMAT; v->format = (int)c->x[1]; }
HLE(tv_target) { RLOCK; View *v = view_of(c->x[0]); v->set |= VS_TARGET; v->target = (int)c->x[1]; }

// ---------------------------------------------------------------- command buffers
// Commands are recorded per guest command buffer between BeginRecording/EndRecording, become an
// immutable list addressed by the returned NVNcommandHandle, and are executed at queue submit.
enum OpKind { OP_RT, OP_CLEAR_COLOR, OP_CLEAR_DS, OP_DRAW, OP_CALL,
              OP_PROGRAM, OP_VATTR, OP_VSTREAM, OP_VB, OP_UBO, OP_TEX, OP_BLEND, OP_COLOR, OP_CMASK, OP_DS, OP_POLY,
              OP_VIEWPORT, OP_SCISSOR, OP_DEPTH_RANGE, OP_BLEND_COLOR, OP_SSBO, OP_CLEAR_COLOR_INT, OP_CLEAR_BUFFER, OP_TEX_POOL, OP_SMP_POOL, OP_STENCIL, OP_STENCIL_DYN, OP_IMAGE, OP_COUNTER, OP_RAST_DISCARD, OP_XFB, OP_BARRIER, OP_POLY_OFFSET, OP_ALPHA_REF };
struct Op;
using OpList = std::vector<Op>;
struct RtData { uint64_t tex[9] = {}; View view[9] = {}; };   // colors[0..7], depth at [8]
struct Op {
    int kind, n = 0;
    float f[4] = {};
    int i[8] = {};
    uint64_t u[3] = {};
    std::shared_ptr<RtData> rt;                              // OP_RT
    std::shared_ptr<std::vector<uint32_t>> words;            // OP_VATTR / OP_VSTREAM payloads
    std::vector<std::shared_ptr<OpList>> calls;              // OP_CALL: nested command lists
};
static std::unordered_map<uint64_t, OpList> g_recording;                 // guest cmd buffer -> ops
static std::unordered_map<uint64_t, std::shared_ptr<OpList>> g_handles;  // handle -> ops
static std::deque<uint64_t> g_handle_order;
// Handles are kept while they may still be submitted: a list used (submitted or called) in the last 300 frames, or
// recorded in them, stays; older ones are released. (Every handle was kept until 65536 of them existed: about 600 KB
// per frame list, 3 MB a frame - the memory leak that slowed the game to a crawl and ended it after a few minutes.)
static std::unordered_map<uint64_t, uint64_t> g_handle_last;   // handle -> last frame recorded / used
static uint64_t g_handles_released, g_handle_misses;
static uint64_t handle_frame();
uint64_t nvn_handles_live() { return g_handles.size(); }   // (memory log)
static void handle_used(uint64_t h) {   // (recording lock held)
    auto it = g_handle_last.find(h);
    if (it != g_handle_last.end()) it->second = handle_frame();
}
static std::mutex g_rec_map_m;   // g_recording's map structure (after the recording lock when both are taken)
static OpList &rec(uint64_t cmd) { std::lock_guard<std::mutex> l(g_rec_map_m); return g_recording[cmd]; }
// A command buffer is recorded by one thread at a time (an NVN rule), and its list never moves (map node): commands
// that only append need no lock - the list is found through a per-thread cache, the map only under the lock.
static OpList &rec_fast(uint64_t cmd) {
    static thread_local uint64_t last_cmd; static thread_local OpList *last;
    if (last && last_cmd == cmd) return *last;
    last = &rec(cmd); last_cmd = cmd; return *last;   // (callers may hold the recording lock: not taken here)
}
static void push(Ctx *c, Op o) { rec_fast(c->x[0]).push_back(std::move(o)); }

HLE(cmd_begin) { RLOCK; rec(c->x[0]).clear(); }
HLE(cmd_end) {
    RLOCK;
    uint64_t h = g_cmd_handles++;
    { static int dbg; if (dbg++ < 0) LOG(1, "[nvn] EndRecording cmd=%llx -> handle %llu (%zu ops)", (unsigned long long)c->x[0], (unsigned long long)h, rec(c->x[0]).size()); }
    OpList &r = rec(c->x[0]);
    size_t n = r.size();
    g_handles[h] = std::make_shared<OpList>(std::move(r));
    r.clear(); r.reserve(n + n / 4 + 16);   // (the next recording of this command buffer is about as long: no regrowth)
    g_handle_order.push_back(h);
    const uint64_t now = handle_frame();
    g_handle_last[h] = now;
    for (int k = 0; k < 16 && g_handle_order.size() > 1; k++) {   // (a few per call, round-robin over the live handles)
        uint64_t o = g_handle_order.front(); g_handle_order.pop_front();
        auto it = g_handle_last.find(o);
        if (it != g_handle_last.end() && it->second + 300 > now) { g_handle_order.push_back(o); continue; }   // still in use
        g_handles.erase(o); if (it != g_handle_last.end()) g_handle_last.erase(it); g_handles_released++;
    }
    RET(h);
}
HLE(cmd_set_rts) {   // (cmd, int numColors, const NVNtexture* const* colors, const NVNtextureView* const* views, const NVNtexture* depth, const NVNtextureView* depthView)
    RLOCK; Op o; o.kind = OP_RT; o.rt = std::make_shared<RtData>(); RtData &r = *o.rt;
    { static int dbg; if (dbg++ < 0) LOG(1, "[nvn] SetRenderTargets n=%d colors=%llx [%llx %llx] views=%llx [%llx] depth=%llx dview=%llx",
        (int)c->x[1], (unsigned long long)c->x[2], (unsigned long long)(c->x[2] ? LD64(c->x[2]) : 0), (unsigned long long)(c->x[2] ? LD64(c->x[2] + 8) : 0),
        (unsigned long long)c->x[3], (unsigned long long)(c->x[3] ? LD64(c->x[3]) : 0), (unsigned long long)c->x[4], (unsigned long long)c->x[5]); }
    int n = (int)c->x[1]; if (n < 0) n = 0; if (n > 8) n = 8; o.n = n;
    for (int i = 0; i < n; i++) {
        r.tex[i] = c->x[2] ? LD64(c->x[2] + 8 * i) : 0;
        uint64_t v = c->x[3] ? LD64(c->x[3] + 8 * i) : 0;
        if (const View *vw = v ? find_view(v) : nullptr) r.view[i] = *vw;
    }
    r.tex[8] = c->x[4];
    if (const View *vw = c->x[5] ? find_view(c->x[5]) : nullptr) r.view[8] = *vw;
    rec(c->x[0]).push_back(o);
}
HLE(cmd_clear_color) {   // (cmd, int index, const float* color, int mask)
    RLOCK; Op o; o.kind = OP_CLEAR_COLOR; o.i[0] = (int)c->x[1]; o.i[1] = (int)c->x[3];
    if (c->x[2]) std::memcpy(o.f, P(2), 16);
    rec(c->x[0]).push_back(o);
}
HLE(cmd_clear_ds) {      // (cmd, float depth, NVNboolean depthMask, int stencil, int stencilMask)
    RLOCK; Op o; o.kind = OP_CLEAR_DS; o.f[0] = c->v[0].f[0];
    o.i[0] = (int)(c->x[1] & 0xff); o.i[1] = (int)c->x[2]; o.i[2] = (int)c->x[3];
    rec(c->x[0]).push_back(o);
}
// memory "used" by the current recording (the game skips submitting command buffers that report 0)
HLE(cmd_mem_used) { RLOCK; RET((uint64_t)(rec(c->x[0]).size() + 1) * 32); }
HLE(cmd_ctrl_used) { RLOCK; RET((uint64_t)(rec(c->x[0]).size() + 1) * 8); }
HLE(cmd_call) {         // nvnCommandBufferCallCommands / CopyCommands (cmd, int count, const NVNcommandHandle* handles)
    RLOCK; Op o; o.kind = OP_CALL;
    for (int i = 0; i < (int)c->x[1]; i++) {
        uint64_t hd = LD64(c->x[2] + 8 * i);
        auto it = g_handles.find(hd);
        if (it != g_handles.end()) { o.calls.push_back(it->second); handle_used(hd); }
        else if (hd && g_handle_misses++ < 8) LOG(1, "[nvn] call of a released command handle %llu", (unsigned long long)hd);
    }
    rec(c->x[0]).push_back(std::move(o));
}
// ---------------------------------------------------------------- texture pool + texture dumps
struct PoolEntry { uint64_t tex; View view; };
// NVN apps can own several texture pools; IDs are per pool and the active one is chosen per command
// buffer with nvnCommandBufferSetTexturePool. Lookups use the active pool, else the last pool registered into.
static std::unordered_map<uint64_t, std::unordered_map<int, PoolEntry>> g_tex_pools;   // pool -> texture ID -> texture
static uint64_t g_cur_tex_pool, g_last_tex_pool;
static const PoolEntry *tex_pool_find(int id) {
    for (uint64_t pool : {g_cur_tex_pool, g_last_tex_pool}) {
        auto p = g_tex_pools.find(pool);
        if (p == g_tex_pools.end()) continue;
        auto it = p->second.find(id);
        if (it != p->second.end()) return &it->second;
    }
    // not in the active pool: take it from any pool (logged once per ID to understand the game's pool use)
    for (auto &p : g_tex_pools) {
        auto it = p.second.find(id);
        if (it == p.second.end()) continue;
        static std::set<int> logged;
        if (logged.insert(id).second)
            LOG(1, "[nvn] texture ID %#x not in active pool %llx (last %llx); found in pool %llx (%zu pools)", id,
                (unsigned long long)g_cur_tex_pool, (unsigned long long)g_last_tex_pool, (unsigned long long)p.first, g_tex_pools.size());
        return &it->second;
    }
    return nullptr;
}
HLE(tp_register_tex) {   // (pool, int id, const NVNtexture*, const NVNtextureView*)
    BOTH; g_tex_gen++; PoolEntry e{c->x[2], View{}};
    if (const View *v = c->x[3] ? find_view(c->x[3]) : nullptr) e.view = *v;
    g_tex_pools[c->x[0]][(int)c->x[1]] = e; g_last_tex_pool = c->x[0];
    static long watch = -2; if (watch == -2) { const char *w = getenv("XRT_WATCH_TEXID"); watch = w ? strtol(w, nullptr, 0) : -1; }
    if (watch >= 0 && (long)c->x[1] == watch) {
        static int n; auto t = g_texs.find(c->x[2]);
        if (n++ < 40) LOG(0, "[nvn] RegisterTexture id %#lx pool %llx tex %llx -> %s view set %x level %d layer %d fmt %d", watch, (unsigned long long)c->x[0],
            (unsigned long long)c->x[2], t == g_texs.end() ? "unknown" : (std::to_string(t->second.b.width) + "x" + std::to_string(t->second.b.height) + " fmt " +
            std::to_string(t->second.b.format) + " lv " + std::to_string(t->second.b.levels) + " addr " + std::to_string(t->second.addr)).c_str(),
            e.view.set, e.view.base_level, e.view.base_layer, e.view.format);
    }
}
HLE(window_set_crop) {   // (window, int x, int y, int w, int h)
    BOTH; static int last[4] = {-1, -1, -1, -1};
    int v[4] = {(int)c->x[1], (int)c->x[2], (int)c->x[3], (int)c->x[4]};
    if (memcmp(v, last, sizeof v)) { memcpy(last, v, sizeof v); LOG(1, "[nvn] window crop %d,%d %dx%d", v[0], v[1], v[2], v[3]); }
    if (gpu_available()) {
        const char *fy = getenv("XRT_FLIP_Y");
        gpu_set_present_crop(v[0], v[1], v[2], v[3], fy ? atoi(fy) != 0 : g_origin_mode == 1);
    }
}
// images (load/store): registered separately from textures, bound per stage/slot
static std::unordered_map<uint64_t, std::unordered_map<int, PoolEntry>> g_img_pools;
HLE(tp_register_image) {   // (pool, int id, const NVNtexture*, const NVNtextureView*)
    BOTH; g_tex_gen++; PoolEntry e{c->x[2], View{}};
    if (const View *v = c->x[3] ? find_view(c->x[3]) : nullptr) e.view = *v;
    g_img_pools[c->x[0]][(int)c->x[1]] = e;
}
HLE(cmd_bind_image) { Op o; o.kind = OP_IMAGE; o.i[0] = (int)c->x[1]; o.i[1] = (int)c->x[2]; o.u[0] = c->x[3]; push(c, o); }
HLE(cmd_bind_images) {   // (cmd, stage, first, count, const NVNimageHandle*)
    for (int i = 0; i < (int)c->x[3]; i++) { Op o; o.kind = OP_IMAGE; o.i[0] = (int)c->x[1]; o.i[1] = (int)c->x[2] + i; o.u[0] = LD64(c->x[4] + 8 * i); push(c, o); }
}
// nvnCommandBufferReportCounter(cmd, NVNcounterType, NVNbufferAddress): the GPU writes {u64 value, u64 timestamp}.
// We don't run real queries: timestamps are host time in Switch ticks, and every other counter (samples passed,
// primitives, invocations) reports a large count, so occlusion/visibility tests made by the game see "visible".
HLE(cmd_report_counter) { Op o; o.kind = OP_COUNTER; o.i[0] = (int)c->x[1]; o.u[0] = c->x[2]; push(c, o); }
HLE(cmd_reset_counter) { }
// rasterizer discard + transform feedback (not executed yet: logged so we can learn the game's usage)
HLE(cmd_rast_discard) { Op o; o.kind = OP_RAST_DISCARD; o.i[0] = (int)(c->x[1] & 0xff); push(c, o); }
HLE(cmd_barrier) { Op o; o.kind = OP_BARRIER; o.u[0] = c->x[1]; push(c, o); }                                       // (cmd, NVNbarrierBits)
HLE(cmd_xfb_bind) { Op o; o.kind = OP_XFB; o.i[0] = 0; o.i[1] = (int)c->x[1]; o.u[0] = c->x[2]; o.u[1] = c->x[3]; push(c, o); }   // (cmd, index, addr, size)
HLE(cmd_xfb_begin) { Op o; o.kind = OP_XFB; o.i[0] = 1; o.u[0] = c->x[1]; push(c, o); }                                       // (cmd, control buffer)
HLE(cmd_xfb_end) { Op o; o.kind = OP_XFB; o.i[0] = 2; o.u[0] = c->x[1]; push(c, o); }
HLE(cmd_xfb_draw) { Op o; o.kind = OP_XFB; o.i[0] = 3; o.i[1] = (int)c->x[1]; o.u[0] = c->x[2]; push(c, o); }                  // (cmd, mode, control buffer)
static bool g_rast_discard;
HLE(cmd_set_tex_pool) { Op o; o.kind = OP_TEX_POOL; o.u[0] = c->x[1]; push(c, o); }
HLE(cmd_set_smp_pool) { Op o; o.kind = OP_SMP_POOL; o.u[0] = c->x[1]; push(c, o); }
// XRT_DUMP_TEXTURES=N: save the first N distinct textures the game binds as textures/*.dds
static long g_dumptex = -2, g_dumped_n;
static std::set<uint64_t> g_dumped;
static std::map<int, int> g_dump_fmts;
static void maybe_dump_texture(uint64_t tex) {
    if (g_dumptex == -2) {
        const char *e = getenv("XRT_DUMP_TEXTURES"); g_dumptex = e ? atol(e) : -1;
#ifdef _WIN32
        if (g_dumptex > 0) _mkdir("textures");
#else
        if (g_dumptex > 0) mkdir("textures", 0755);
#endif
    }
    if (g_dumptex <= 0 || g_dumped_n >= g_dumptex || !tex || !g_dumped.insert(tex).second) return;
    auto it = g_texs.find(tex);
    if (it == g_texs.end()) return;
    const Tex &t = it->second; const Builder &b = t.b;
    g_dump_fmts[b.format]++;
    const NvnFormat *f = nvn_format(b.format);
    if (!f || f->depth || b.target == 2) return;             // unknown, depth, or 3D
    int wb = (b.width + f->bw - 1) / f->bw, hb = (b.height + f->bh - 1) / f->bh;
    int bh = bl_block_height(hb);
    uint64_t src = b.packaged ? b.packaged : t.addr;
    if (!src) return;
    if (!b.packaged) {      // never read past the end of the memory pool the texture lives in
        auto pit = g_pools.find(b.pool);
        if (pit == g_pools.end() || b.offset + bl_level_size(wb, hb, f->bpb, bh) > pit->second.size) {
            LOG(1, "[nvn] texture %llx (%s %dx%d) doesn't fit its pool, not dumped", (unsigned long long)tex, f->name, b.width, b.height);
            return;
        }
    }
    std::vector<uint8_t> lin((size_t)wb * hb * f->bpb);
    bl_deswizzle((const uint8_t *)(uintptr_t)src, lin.data(), wb, hb, f->bpb, bh);
    char name[160];
    snprintf(name, sizeof name, "textures/%04ld_%s_%dx%d_%llx.dds", g_dumped_n, f->name, b.width, b.height, (unsigned long long)tex);
    if (write_dds(name, *f, b.width, b.height, lin.data(), lin.size())) g_dumped_n++;
    if (g_dumped_n == g_dumptex) {
        std::string s; for (auto &kv : g_dump_fmts) { char x[48]; snprintf(x, sizeof x, " %s:%d", nvn_format_name(kv.first), kv.second); s += x; }
        LOG(1, "[nvn] dumped %ld textures; formats seen:%s", g_dumped_n, s.c_str());
    }
}
HLE(cmd_bind_texture) {  // (cmd, NVNshaderStage stage, int index, NVNtextureHandle handle)
    int id = (int)(c->x[3] & 0xfffff);
    if (g_dumptex != -1) { NLOCK; if (const PoolEntry *e = tex_pool_find(id)) maybe_dump_texture(e->tex); }   // (XRT_DUMP_TEXTURES only)
    Op o; o.kind = OP_TEX; o.i[0] = (int)c->x[1]; o.i[1] = (int)c->x[2]; o.u[0] = c->x[3]; push(c, o);
}
// ---------------------------------------------------------------- programs / shader capture
// nvnProgramSetShaders(NVNprogram*, int count, const NVNshaderData*), NVNshaderData = { u64 gpuAddress; const void* control; }
// XRT_DUMP_SHADERS=1: write every distinct shader (control + code head) to shaders/ for analysis.
// control section (CPU side, compiled by NVN's offline compiler): offset 0x700/0x704 = start/end of the
// shader's literal constants inside the code memory; the shader reads them as constant buffer bank 1.
struct ShaderStage { uint64_t code, control; uint32_t const_off = 0, const_end = 0; };
static std::unordered_map<uint64_t, std::vector<ShaderStage>> g_programs;
static int g_dumpsh = -1; static std::set<uint64_t> g_sh_seen; static int g_sh_n;
static void dump_shader(const ShaderStage &st) {
    if (g_dumpsh < 0) {
        g_dumpsh = getenv("XRT_DUMP_SHADERS") ? 1 : 0;
#ifdef _WIN32
        if (g_dumpsh) _mkdir("shaders");
#else
        if (g_dumpsh) mkdir("shaders", 0755);
#endif
    }
    if (!g_dumpsh || !g_sh_seen.insert(st.control).second) return;
    char name[96];
    snprintf(name, sizeof name, "shaders/%04d_%llx.ctrl", g_sh_n, (unsigned long long)st.control);
    if (FILE *f = fopen(name, "wb")) { fwrite((void *)(uintptr_t)st.control, 1, 0x1000, f); fclose(f); }
    // code: up to and including the final "BRA self" marker, never past the end of its memory pool
    uint64_t limit = 0;
    for (auto &kv : g_pools)
        if (st.code >= kv.second.storage && st.code < kv.second.storage + kv.second.size) limit = kv.second.storage + kv.second.size;
    if (!limit) return;
    uint64_t end = st.code + 0x80;
    while (end + 8 <= limit && end < st.code + (1 << 20)) { end += 8; if (LD64(end - 8) == 0xE2400FFFFF87000FULL) break; }
    snprintf(name, sizeof name, "shaders/%04d_%llx.code", g_sh_n, (unsigned long long)st.code);
    if (FILE *f = fopen(name, "wb")) { fwrite((void *)(uintptr_t)st.code, 1, end - st.code, f); fclose(f); }
    g_sh_n++;
}
HLE(prog_set_shaders) {
    BOTH; std::vector<ShaderStage> v;
    for (int i = 0; i < (int)c->x[1]; i++) {
        ShaderStage st{LD64(c->x[2] + 16 * i), LD64(c->x[2] + 16 * i + 8)};
        if (st.control && LD32(st.control) == 0x98761234) { st.const_off = LD32(st.control + 0x700); st.const_end = LD32(st.control + 0x704); }
        if (st.const_end > st.const_off && getenv("XRT_LOG_CONSTS")) {
            static int n; if (n++ < 6) { const float *f = (const float *)(uintptr_t)(st.code + st.const_off);
                LOG(0, "[nvn] shader constants @+%#x (%u bytes): %g %g %g %g %g %g %g %g", st.const_off, st.const_end - st.const_off, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]); }
        }
        v.push_back(st); dump_shader(st);
    }
    g_programs[c->x[0]] = v;
    RET(1);
}
// ---------------------------------------------------------------- render state objects
// Set by the game through setters on its own small structs, then bound by address. As with texture
// views, the values live in side tables keyed by the struct address (never written into guest memory).
struct VAttr { int format = 0; uint32_t offset = 0; int stream = 0; };
struct VStream { uint32_t stride = 0, divisor = 0; };
struct BlendSt { int target = 0; GpuBlend b; };
struct ColorSt { uint8_t enable = 0; int logic_op = 3; int alpha_func = 8; };
struct CMaskSt { uint8_t mask[8] = {15, 15, 15, 15, 15, 15, 15, 15}; };
struct DepthSt { bool test = false, write = false, stencil = false; int func = 2; int sfunc[2] = {8, 8}, sop[2][3] = {{1, 1, 1}, {1, 1, 1}}; };
struct PolySt { int cull = 0, front = 1, mode = 2, offset_enables = 0; };
static std::unordered_map<uint64_t, VAttr> g_vattr;     // NVNvertexAttribState is 4 bytes
static std::unordered_map<uint64_t, VStream> g_vstream;
static std::unordered_map<uint64_t, BlendSt> g_blend;
static std::unordered_map<uint64_t, ColorSt> g_color;
static std::unordered_map<uint64_t, CMaskSt> g_cmask;
static std::unordered_map<uint64_t, DepthSt> g_depth;
static std::unordered_map<uint64_t, PolySt> g_poly;
// NVNvertexAttribState is a 4-byte object the game may copy around (build once, memcpy into arrays), so like
// the real driver the setters also store the state in it: 0xA5 tag | stream << 20 | format << 12 | offset.
static uint32_t va_encode(const VAttr &a) { return 0xA5000000u | ((uint32_t)a.stream & 15) << 20 | ((uint32_t)a.format & 0xff) << 12 | (a.offset & 0xfff); }
static bool va_decode(uint32_t w, VAttr &a) {
    if ((w >> 24) != 0xA5) return false;
    a.stream = (int)((w >> 20) & 15); a.format = (int)((w >> 12) & 0xff); a.offset = w & 0xfff; return true;
}
static void va_store(uint64_t addr) { *(uint32_t *)(uintptr_t)addr = va_encode(g_vattr[addr]); }
HLE(va_defaults) { RLOCK; g_vattr[c->x[0]] = VAttr{}; va_store(c->x[0]); }
HLE(va_format) { RLOCK; VAttr &a = g_vattr[c->x[0]]; a.format = (int)c->x[1]; a.offset = (uint32_t)c->x[2]; va_store(c->x[0]); }
HLE(va_stream) { RLOCK; g_vattr[c->x[0]].stream = (int)c->x[1]; va_store(c->x[0]); }
// NVNvertexStreamState (8 bytes) gets the same treatment: 0xA6 tag | stride (24 bits), then the divisor.
static void vs_store(uint64_t addr) { const VStream &v = g_vstream[addr]; uint32_t *w = (uint32_t *)(uintptr_t)addr; w[0] = 0xA6000000u | (v.stride & 0xffffff); w[1] = v.divisor; }
static bool vs_decode(const uint32_t *w, VStream &v) { if ((w[0] >> 24) != 0xA6) return false; v.stride = w[0] & 0xffffff; v.divisor = w[1]; return true; }
HLE(vs_defaults) { RLOCK; g_vstream[c->x[0]] = VStream{}; vs_store(c->x[0]); }
HLE(vs_stride) { RLOCK; g_vstream[c->x[0]].stride = (uint32_t)c->x[1]; vs_store(c->x[0]); }
HLE(vs_divisor) { RLOCK; g_vstream[c->x[0]].divisor = (uint32_t)c->x[1]; vs_store(c->x[0]); }
// Decoded attribute / stream arrays, shared by every bind of the same encoded bytes (the game binds a few layouts
// thousands of times a frame): no allocation and no map lookups per bind.
using Words = std::shared_ptr<std::vector<uint32_t>>;
struct WordsCache { std::unordered_map<uint64_t, std::pair<std::vector<uint8_t>, Words>> m; };
static Words words_cached(WordsCache &wc, const void *raw, size_t n, const std::vector<uint32_t> &decoded) {
    const uint8_t *b = (const uint8_t *)raw; uint64_t h = 0xcbf29ce484222325ull ^ n;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    auto it = wc.m.find(h);
    if (it != wc.m.end() && it->second.first.size() == n && !memcmp(it->second.first.data(), b, n)) return it->second.second;
    Words w = std::make_shared<std::vector<uint32_t>>(decoded);
    if (it == wc.m.end()) wc.m.emplace(h, std::make_pair(std::vector<uint8_t>(b, b + n), w));
    return w;
}
static WordsCache g_vattr_words, g_vstream_words;
HLE(bs_defaults) { RLOCK; g_blend[c->x[0]] = BlendSt{}; }
HLE(bs_target) { RLOCK; g_blend[c->x[0]].target = (int)c->x[1]; }
HLE(bs_func) { RLOCK; GpuBlend &b = g_blend[c->x[0]].b; b.src_c = (int)c->x[1]; b.dst_c = (int)c->x[2]; b.src_a = (int)c->x[3]; b.dst_a = (int)c->x[4]; }
HLE(bs_equation) { RLOCK; GpuBlend &b = g_blend[c->x[0]].b; b.eq_c = (int)c->x[1]; b.eq_a = (int)c->x[2]; }
HLE(cs_defaults) { RLOCK; g_color[c->x[0]] = ColorSt{}; }
HLE(cs_blend_enable) { RLOCK; ColorSt &cs = g_color[c->x[0]]; int i = (int)c->x[1] & 7; if (c->x[2] & 0xff) cs.enable |= 1 << i; else cs.enable &= ~(1 << i); }
HLE(cs_get_blend_enable) { RLOCK; RET((g_color[c->x[0]].enable >> ((int)c->x[1] & 7)) & 1); }
HLE(cs_logic_op) { RLOCK; g_color[c->x[0]].logic_op = (int)c->x[1]; }
HLE(cs_alpha_test) { RLOCK; g_color[c->x[0]].alpha_func = (int)c->x[1]; }
HLE(cm_defaults) { RLOCK; g_cmask[c->x[0]] = CMaskSt{}; }
HLE(cm_mask) { RLOCK; g_cmask[c->x[0]].mask[(int)c->x[1] & 7] = (uint8_t)((c->x[2] & 1) | (c->x[3] & 1) << 1 | (c->x[4] & 1) << 2 | (c->x[5] & 1) << 3); }
HLE(ds_defaults) { RLOCK; g_depth[c->x[0]] = DepthSt{}; }
HLE(ds_test) { RLOCK; g_depth[c->x[0]].test = c->x[1] & 0xff; }
HLE(ds_write) { RLOCK; g_depth[c->x[0]].write = c->x[1] & 0xff; }
HLE(ds_func) { RLOCK; g_depth[c->x[0]].func = (int)c->x[1]; }
HLE(ds_stencil) { RLOCK; g_depth[c->x[0]].stencil = c->x[1] & 0xff; }
// NVNface: 1 front, 2 back, 3 both
HLE(ds_stencil_func) { RLOCK; DepthSt &d = g_depth[c->x[0]]; for (int f = 0; f < 2; f++) if (c->x[1] & (1 << f)) d.sfunc[f] = (int)c->x[2]; }
HLE(ds_stencil_op) {
    RLOCK; DepthSt &d = g_depth[c->x[0]];
    for (int f = 0; f < 2; f++) if (c->x[1] & (1 << f)) { d.sop[f][0] = (int)c->x[2]; d.sop[f][1] = (int)c->x[3]; d.sop[f][2] = (int)c->x[4]; }
}
HLE(cmd_stencil_ref) { Op o; o.kind = OP_STENCIL_DYN; o.i[0] = 0; o.i[1] = (int)c->x[1]; o.i[2] = (int)c->x[2]; push(c, o); }
HLE(cmd_stencil_vmask) { Op o; o.kind = OP_STENCIL_DYN; o.i[0] = 1; o.i[1] = (int)c->x[1]; o.i[2] = (int)c->x[2]; push(c, o); }
HLE(cmd_stencil_wmask) { Op o; o.kind = OP_STENCIL_DYN; o.i[0] = 2; o.i[1] = (int)c->x[1]; o.i[2] = (int)c->x[2]; push(c, o); }
HLE(ps_defaults) { RLOCK; g_poly[c->x[0]] = PolySt{}; }
HLE(ps_cull) { RLOCK; g_poly[c->x[0]].cull = (int)c->x[1]; }
HLE(ps_front) { RLOCK; g_poly[c->x[0]].front = (int)c->x[1]; }
HLE(ps_mode) { RLOCK; g_poly[c->x[0]].mode = (int)c->x[1]; }
HLE(ps_offset_enables) { RLOCK; g_poly[c->x[0]].offset_enables = (int)c->x[1]; }

// samplers: builder fields -> sampler object -> sampler pool ID
static std::unordered_map<uint64_t, GpuSampler> g_sampler_builders, g_samplers;
static std::unordered_map<uint64_t, std::unordered_map<uint32_t, GpuSampler>> g_sampler_pools;
static uint64_t g_cur_smp_pool, g_last_smp_pool;
HLE(sb_defaults) { RLOCK; g_sampler_builders[c->x[0]] = GpuSampler{}; }
HLE(sb_wrap) { RLOCK; GpuSampler &sm = g_sampler_builders[c->x[0]]; sm.wrap[0] = (int)c->x[1]; sm.wrap[1] = (int)c->x[2]; sm.wrap[2] = (int)c->x[3]; }
HLE(sb_minmag) { RLOCK; GpuSampler &sm = g_sampler_builders[c->x[0]]; sm.min = (int)c->x[1]; sm.mag = (int)c->x[2]; }
HLE(sb_aniso) { RLOCK; g_sampler_builders[c->x[0]].max_aniso = c->v[0].f[0]; }
HLE(sb_lod_clamp) { RLOCK; GpuSampler &sm = g_sampler_builders[c->x[0]]; sm.lod_min = c->v[0].f[0]; sm.lod_max = c->v[1].f[0]; }
HLE(sb_lod_bias) { RLOCK; g_sampler_builders[c->x[0]].lod_bias = c->v[0].f[0]; }
HLE(sb_border) { RLOCK; std::memcpy(g_sampler_builders[c->x[0]].border, P(1), 16); }   // (builder, const float/int/uint[4]): raw bits kept
HLE(sb_compare) { RLOCK; GpuSampler &sm = g_sampler_builders[c->x[0]]; sm.compare = (int)c->x[1]; sm.compare_func = (int)c->x[2]; }
HLE(sampler_init) { RLOCK; g_samplers[c->x[0]] = g_sampler_builders[c->x[1]]; RET(1); }
HLE(sp_register_sampler) {   // (pool, int id, const NVNsampler*)
    BOTH; g_tex_gen++; auto it = g_samplers.find(c->x[2]);
    g_sampler_pools[c->x[0]][(uint32_t)c->x[1]] = it != g_samplers.end() ? it->second : GpuSampler{}; g_last_smp_pool = c->x[0];
}

// ---------------------------------------------------------------- recorded binds
// nvnCommandBufferBindProgram(cmd, program, NVNshaderStageBits stages): only the stages in the mask are
// replaced (VERTEX 1, FRAGMENT 2, GEOMETRY 4, TESS_CONTROL 8, TESS_EVALUATION 0x10); the game binds a shared
// vertex program and then per-material fragment-only programs.
HLE(cmd_bind_program) { Op o; o.kind = OP_PROGRAM; o.u[0] = c->x[1]; o.u[1] = (uint32_t)c->x[2]; push(c, o); }
static void state_miss(const char *what, uint64_t a) {   // a state object bound but never configured through our setters
    static std::set<std::string> seen; if (seen.insert(what).second) LOG(1, "[nvn] %s state %llx was never set up through its setters (copied object?) - using defaults", what, (unsigned long long)a);
}
HLE(cmd_bind_vattr) {    // (cmd, int num, const NVNvertexAttribState* attribs)
    RLOCK; Op o; o.kind = OP_VATTR;
    int n = (int)c->x[1]; if (n > 16) n = 16; if (n < 0) n = 0; o.n = n;
    {   // fast path: every state carries our encoding (set through the setters, or a copy of one)
        const uint32_t *raw = (const uint32_t *)(uintptr_t)c->x[2];
        uint32_t dec[48]; bool all = raw != nullptr;
        for (int i = 0; i < n && all; i++) { VAttr a; if (!va_decode(raw[i], a)) all = false; else { dec[3 * i] = (uint32_t)a.format; dec[3 * i + 1] = a.offset; dec[3 * i + 2] = (uint32_t)a.stream; } }
        if (all) {
            static thread_local std::vector<uint32_t> tmp; tmp.assign(dec, dec + 3 * n);
            o.words = words_cached(g_vattr_words, raw, (size_t)n * 4, tmp);
            push(c, std::move(o));
            return;
        }
    }
    o.words = std::make_shared<std::vector<uint32_t>>();
    for (int i = 0; i < n; i++) {
        auto it = g_vattr.find(c->x[2] + 4 * i);
        VAttr a = it != g_vattr.end() ? it->second : VAttr{};
        if (it == g_vattr.end() && !va_decode(LD32(c->x[2] + 4 * i), a)) {   // neither set here nor a copy of a set state
            static int n; if (n++ < 8) LOG(1, "[nvn] vertex attrib state %llx unknown (word %08x)", (unsigned long long)(c->x[2] + 4 * i), LD32(c->x[2] + 4 * i));
        }
        o.words->push_back((uint32_t)a.format); o.words->push_back(a.offset); o.words->push_back((uint32_t)a.stream);
    }
    push(c, std::move(o));
}
HLE(cmd_bind_vstream) {  // (cmd, int num, const NVNvertexStreamState* streams) - 8 bytes each
    RLOCK; Op o; o.kind = OP_VSTREAM;
    int n = (int)c->x[1]; if (n > 16) n = 16; if (n < 0) n = 0; o.n = n;
    {
        const uint32_t *raw = (const uint32_t *)(uintptr_t)c->x[2];
        uint32_t dec[32]; bool all = raw != nullptr;
        for (int i = 0; i < n && all; i++) { VStream v; if (!vs_decode(raw + 2 * i, v)) all = false; else { dec[2 * i] = v.stride; dec[2 * i + 1] = v.divisor; } }
        if (all) {
            static thread_local std::vector<uint32_t> tmp; tmp.assign(dec, dec + 2 * n);
            o.words = words_cached(g_vstream_words, raw, (size_t)n * 8, tmp);
            push(c, std::move(o));
            return;
        }
    }
    o.words = std::make_shared<std::vector<uint32_t>>();
    for (int i = 0; i < n; i++) {
        auto it = g_vstream.find(c->x[2] + 8 * i);
        VStream v = it != g_vstream.end() ? it->second : VStream{};
        if (it == g_vstream.end()) state_miss("vertex stream", c->x[2] + 8 * i);
        o.words->push_back(v.stride); o.words->push_back(v.divisor);
    }
    push(c, std::move(o));
}
HLE(cmd_bind_vb) { Op o; o.kind = OP_VB; o.i[0] = (int)c->x[1]; o.u[0] = c->x[2]; o.u[1] = c->x[3]; push(c, o); }
HLE(cmd_bind_vbs) {      // (cmd, int first, int count, const NVNbufferRange*)
    for (int i = 0; i < (int)c->x[2]; i++) { Op o; o.kind = OP_VB; o.i[0] = (int)c->x[1] + i; o.u[0] = LD64(c->x[3] + 16 * i); o.u[1] = LD64(c->x[3] + 16 * i + 8); push(c, o); }
}
HLE(cmd_bind_ubo) { Op o; o.kind = OP_UBO; o.i[0] = (int)c->x[1]; o.i[1] = (int)c->x[2]; o.u[0] = c->x[3]; o.u[1] = c->x[4]; push(c, o); }
HLE(cmd_bind_ubos) {     // (cmd, stage, int first, int count, const NVNbufferRange*)
    for (int i = 0; i < (int)c->x[3]; i++) { Op o; o.kind = OP_UBO; o.i[0] = (int)c->x[1]; o.i[1] = (int)c->x[2] + i; o.u[0] = LD64(c->x[4] + 16 * i); o.u[1] = LD64(c->x[4] + 16 * i + 8); push(c, o); }
}
HLE(cmd_bind_textures) { // (cmd, stage, int first, int count, const NVNtextureHandle*)
    for (int i = 0; i < (int)c->x[3]; i++) { Op o; o.kind = OP_TEX; o.i[0] = (int)c->x[1]; o.i[1] = (int)c->x[2] + i; o.u[0] = LD64(c->x[4] + 8 * i); push(c, o); }
}
HLE(cmd_bind_blend) {
    RLOCK; auto it = g_blend.find(c->x[1]); if (it == g_blend.end()) state_miss("blend", c->x[1]); BlendSt b = it != g_blend.end() ? it->second : BlendSt{};
    Op o; o.kind = OP_BLEND; o.i[0] = b.target; o.i[1] = b.b.src_c; o.i[2] = b.b.dst_c; o.i[3] = b.b.src_a; o.i[4] = b.b.dst_a; o.i[5] = b.b.eq_c; o.i[6] = b.b.eq_a; push(c, o);
}
HLE(cmd_bind_color) { RLOCK; auto it = g_color.find(c->x[1]); if (it == g_color.end()) state_miss("color", c->x[1]); ColorSt cs = it != g_color.end() ? it->second : ColorSt{}; Op o; o.kind = OP_COLOR; o.i[0] = cs.enable; o.i[1] = cs.logic_op; o.i[2] = cs.alpha_func; push(c, o); }
HLE(cmd_bind_cmask) { RLOCK; auto it = g_cmask.find(c->x[1]); if (it == g_cmask.end()) state_miss("channel mask", c->x[1]); CMaskSt m = it != g_cmask.end() ? it->second : CMaskSt{}; Op o; o.kind = OP_CMASK; for (int i = 0; i < 8; i++) o.i[i] = m.mask[i]; push(c, o); }
HLE(cmd_bind_depth) { RLOCK; auto it = g_depth.find(c->x[1]); if (it == g_depth.end()) state_miss("depth-stencil", c->x[1]); DepthSt d = it != g_depth.end() ? it->second : DepthSt{}; Op o; o.kind = OP_DS; o.i[0] = d.test; o.i[1] = d.write; o.i[2] = d.func; o.i[3] = d.stencil; push(c, o);
    Op so; so.kind = OP_STENCIL; so.i[0] = d.sfunc[0]; so.i[1] = d.sfunc[1];
    for (int k = 0; k < 3; k++) { so.i[2 + k] = d.sop[0][k]; so.i[5 + k] = d.sop[1][k]; }
    push(c, so); }
HLE(cmd_bind_poly) { RLOCK; auto it = g_poly.find(c->x[1]); if (it == g_poly.end()) state_miss("polygon", c->x[1]); PolySt p = it != g_poly.end() ? it->second : PolySt{}; Op o; o.kind = OP_POLY; o.i[0] = p.cull; o.i[1] = p.front; o.i[2] = p.mode; o.i[3] = p.offset_enables; push(c, o); }
HLE(cmd_viewport) { Op o; o.kind = OP_VIEWPORT; for (int i = 0; i < 4; i++) o.i[i] = (int)c->x[1 + i]; push(c, o); }
HLE(cmd_viewports) {     // (cmd, int first, int count, const float* xywh)
    if (c->x[1] == 0 && c->x[2] >= 1) { Op o; o.kind = OP_VIEWPORT; const float *v = (const float *)P(3); for (int i = 0; i < 4; i++) o.i[i] = (int)v[i]; push(c, o); }
}
HLE(cmd_scissor) { Op o; o.kind = OP_SCISSOR; for (int i = 0; i < 4; i++) o.i[i] = (int)c->x[1 + i]; push(c, o); }
HLE(cmd_scissors) {      // (cmd, int first, int count, const int* xywh)
    if (c->x[1] == 0 && c->x[2] >= 1) { Op o; o.kind = OP_SCISSOR; const int32_t *v = (const int32_t *)P(3); for (int i = 0; i < 4; i++) o.i[i] = v[i]; push(c, o); }
}
HLE(cmd_bind_ssbo) { Op o; o.kind = OP_SSBO; o.i[0] = (int)c->x[1]; o.i[1] = (int)c->x[2]; o.u[0] = c->x[3]; o.u[1] = c->x[4]; push(c, o); }
HLE(cmd_bind_ssbos) {    // (cmd, stage, int first, int count, const NVNbufferRange*)
    for (int i = 0; i < (int)c->x[3]; i++) { Op o; o.kind = OP_SSBO; o.i[0] = (int)c->x[1]; o.i[1] = (int)c->x[2] + i; o.u[0] = LD64(c->x[4] + 16 * i); o.u[1] = LD64(c->x[4] + 16 * i + 8); push(c, o); }
}
HLE(cmd_clear_color_int) {   // ClearColori / ClearColorui (cmd, int index, const int* color, int mask)
    RLOCK; Op o; o.kind = OP_CLEAR_COLOR_INT; o.i[0] = (int)c->x[1]; o.i[1] = (int)c->x[3];
    if (c->x[2]) std::memcpy(&o.i[2], P(2), 16);
    push(c, o);
}
HLE(cmd_clear_buffer) { Op o; o.kind = OP_CLEAR_BUFFER; o.u[0] = c->x[1]; o.u[1] = c->x[2]; o.i[0] = (int)c->x[3]; push(c, o); }
HLE(cmd_depth_range) { Op o; o.kind = OP_DEPTH_RANGE; o.f[0] = c->v[0].f[0]; o.f[1] = c->v[1].f[0]; push(c, o); }
HLE(cmd_poly_offset) { Op o; o.kind = OP_POLY_OFFSET; for (int i = 0; i < 3; i++) o.f[i] = c->v[i].f[0]; push(c, o); }   // (cmd, factor, units, clamp)
HLE(cmd_alpha_ref) { Op o; o.kind = OP_ALPHA_REF; o.f[0] = c->v[0].f[0]; push(c, o); }
HLE(cmd_blend_color) { Op o; o.kind = OP_BLEND_COLOR; std::memcpy(o.f, P(1), 16); push(c, o); }

// draws: (cmd, mode, first, count) / (cmd, mode, type, count, indexAddr) / ... see the NVN signatures below
HLE(cmd_draw_arrays) { Op o; o.kind = OP_DRAW; o.i[0] = (int)c->x[1]; o.i[2] = (int)c->x[3]; o.i[3] = (int)c->x[2]; o.i[7] = 1; push(c, o); }
HLE(cmd_draw_arrays_inst) {   // (cmd, mode, first, count, baseInstance, instanceCount)
    Op o; o.kind = OP_DRAW; o.i[0] = (int)c->x[1]; o.i[3] = (int)c->x[2]; o.i[2] = (int)c->x[3]; o.i[6] = (int)c->x[4]; o.i[7] = (int)c->x[5]; push(c, o);
}
HLE(cmd_draw_elements) {      // (cmd, mode, type, count, indexBuffer)
    Op o; o.kind = OP_DRAW; o.n = 1; o.i[0] = (int)c->x[1]; o.i[1] = (int)c->x[2]; o.i[2] = (int)c->x[3]; o.u[0] = c->x[4]; o.i[7] = 1; push(c, o);
}
HLE(cmd_draw_elements_bv) {   // (cmd, mode, type, count, indexBuffer, baseVertex)
    Op o; o.kind = OP_DRAW; o.n = 1; o.i[0] = (int)c->x[1]; o.i[1] = (int)c->x[2]; o.i[2] = (int)c->x[3]; o.u[0] = c->x[4]; o.i[5] = (int)c->x[5]; o.i[7] = 1; push(c, o);
}
HLE(cmd_draw_elements_inst) { // (cmd, mode, type, count, indexBuffer, baseVertex, baseInstance, instanceCount)
    Op o; o.kind = OP_DRAW; o.n = 1; o.i[0] = (int)c->x[1]; o.i[1] = (int)c->x[2]; o.i[2] = (int)c->x[3]; o.u[0] = c->x[4];
    o.i[5] = (int)c->x[5]; o.i[6] = (int)c->x[6]; o.i[7] = (int)c->x[7]; push(c, o);
}

// ---------------------------------------------------------------- execution
static struct { uint64_t color[8]; View cview[8]; int n; uint64_t depth; View dview; } g_rt;
// XRT_NVNLOG=N: log every op of the first N frames; XRT_NVNLOG=FROM,N: of frames FROM..FROM+N-1
static int g_nvnlog = -1, g_nvnlog_from = 0;
static bool log_frame() {
    if (g_nvnlog < 0) {
        const char *e = getenv("XRT_NVNLOG"); g_nvnlog = 0;
        if (e) { if (strchr(e, ',')) sscanf(e, "%d,%d", &g_nvnlog_from, &g_nvnlog); else g_nvnlog = atoi(e); }
    }
    int64_t f = (int64_t)g_frames.load();
    return f >= g_nvnlog_from && f < g_nvnlog_from + g_nvnlog;
}
static std::string tex_desc(uint64_t a) {
    auto it = g_texs.find(a);
    if (it == g_texs.end()) return a ? "?" : "-";
    const Builder &b = it->second.b; char s[96];
    snprintf(s, sizeof s, "%llx[%s %dx%dx%d t%d]", (unsigned long long)a, gpu_format_name(b.format), b.width, b.height, b.depth, b.target);
    return s;
}
static void upload_texture(Tex &t);
// as_rt: the texture is used as a render target (its memory holds no image data to upload)
// Block-linear layout of one level: byte offset inside a layer (2D/arrays) or the whole texture (3D),
// GOB block height/depth, and the layer stride for arrays.
struct LevelLayout { uint64_t off = 0, layer_size = 0; int wb = 1, hb = 1, d = 1, bh = 1, bd = 1; };
static bool level_layout(const Builder &b, const NvnFormat *f, int level, LevelLayout &L) {
    int levels = b.levels > 0 ? b.levels : 1;
    if (level < 0 || level >= levels) return false;
    int hb0 = (b.height + f->bh - 1) / f->bh;
    int bh0 = bl_block_height(hb0);
    bool is3d = b.target == 2;
    int d0 = is3d ? (b.depth > 0 ? b.depth : 1) : 1, bd0 = is3d ? bl_block_depth(d0) : 1;
    while (bh0 > 1 && bh0 * bd0 > 16) bh0 /= 2;   // same 3D rule as upload_texture
    uint64_t off = 0;
    for (int l = 0; l < levels; l++) {
        int w = std::max(1, b.width >> l), h = std::max(1, b.height >> l), dz = std::max(1, d0 >> l);
        int wb = (w + f->bw - 1) / f->bw, hb = (h + f->bh - 1) / f->bh;
        int bh = bh0; while (bh > 1 && hb <= (bh / 2) * 8) bh /= 2;
        int bd = bd0; while (bd > 1 && dz <= bd / 2) bd /= 2;
        uint64_t sz = is3d ? bl_level_size_3d(wb, hb, dz, f->bpb, bh, bd) : bl_level_size(wb, hb, f->bpb, bh);
        if (l == level) { L.off = off; L.wb = wb; L.hb = hb; L.d = dz; L.bh = bh; L.bd = bd; }
        off += sz;
    }
    L.layer_size = off;
    int layers = is3d ? 1 : (b.depth > 0 ? b.depth : 1);
    if (layers > 1) { uint64_t al = (uint64_t)bh0 * 512; L.layer_size = (L.layer_size + al - 1) / al * al; }
    return true;
}
static uint64_t mem_hash(uint64_t addr, uint32_t n) {
    const uint8_t *p = (const uint8_t *)(uintptr_t)addr; uint64_t h = 1469598103934665603ULL;
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t v; memcpy(&v, p + i, 8); h = (h ^ v) * 1099511628211ULL; h ^= h >> 29; }
    for (; i < n; i++) h = (h ^ p[i]) * 1099511628211ULL;
    return h;
}
static uint32_t small_tex_bytes(const Tex &t) {   // block-linear footprint if it's small enough to watch (<= 64 KB), else 0
    const NvnFormat *f = nvn_format(t.b.format);
    LevelLayout L;
    if (!f || f->depth || t.b.packaged || !t.addr || !level_layout(t.b, f, 0, L)) return 0;
    uint64_t n = L.layer_size * (uint64_t)(t.b.target == 2 ? 1 : std::max(1, t.b.depth));
    auto pit = g_pools.find(t.b.pool);       // never read past the texture's memory pool
    if (pit == g_pools.end() || t.addr < pit->second.storage || t.addr + n > pit->second.storage + pit->second.size) return 0;
    return n <= 65536 ? (uint32_t)n : 0;
}
static GpuTex *gpu_tex(uint64_t a, bool as_rt = false) {
    auto it = g_texs.find(a);
    if (it == g_texs.end()) return nullptr;
    Tex &t = it->second;
    if (as_rt) t.is_rt = true;
    // small sampled textures in CPU-visible memory: the game may rewrite their texels directly (e.g. 1x1
    // parameter textures); re-upload when the bytes change (checked at most once per frame)
    if (t.gpu && !as_rt && !t.is_rt && !t.alias && t.mem_bytes) {
        uint64_t f = g_frames.load();
        if (t.hash_frame != f) {
            t.hash_frame = f;
            uint64_t h = mem_hash(t.addr, t.mem_bytes);
            if (h != t.mem_hash) { t.mem_hash = h; t.dirty = true; }
        }
    }
    if (!t.gpu && !t.gpu_tried && !t.b.packaged && t.addr) {
        // Several NVN textures can describe the same memory (e.g. a depth-stencil buffer and a stencil-mode
        // texture over it for sampling): share one GPU image so the sampler sees what was rendered.
        const NvnFormat *f = nvn_format(t.b.format);
        for (auto &kv : g_texs) {
            Tex &o = kv.second;
            if (&o == &t || !o.gpu || o.alias || o.addr != t.addr || o.b.width != t.b.width || o.b.height != t.b.height) continue;
            const NvnFormat *of = nvn_format(o.b.format);
            if (!f || !of || (f->depth != 0) != (of->depth != 0) || (!f->depth && f->bpb != of->bpb)) continue;
            t.gpu = o.gpu; t.alias = true; t.gpu_tried = true; o.shared = true;
            return t.gpu;
        }
    }
    if (t.gpu && t.dirty) { t.dirty = false; if (!t.alias) upload_texture(t); }   // CPU wrote texels since the last upload
    if (!t.gpu && !t.gpu_tried) {
        t.gpu_tried = true;
        t.dirty = false;
        int layers = t.b.depth;                                   // array layers, or slices of a 3D texture
        GpuTexDesc d{t.b.format, t.b.width, t.b.height, layers, t.b.levels, t.b.samples, t.b.target};
        t.gpu = gpu_tex_create(d);
        if (t.gpu && !as_rt) {
            upload_texture(t);
            t.mem_bytes = small_tex_bytes(t);
            if (t.mem_bytes) { t.mem_hash = mem_hash(t.addr, t.mem_bytes); t.hash_frame = g_frames.load(); }
        }
    }
    return t.gpu;
}
// Upload every level/layer of a block-linear texture (the layout the game's files and the GPU use)
// nvnTextureWriteTexels[Strided](tex, view, const NVNcopyRegion*, data[, rowStride, imageStride]):
// CPU writes into the texture's memory (block-linear), then the GPU copy is refreshed.
static void write_texels(uint64_t tex, uint64_t view, uint64_t region, uint64_t data, uint64_t row_stride, uint64_t image_stride) {
    auto it = g_texs.find(tex);
    if (it == g_texs.end() || !region || !data) return;
    Tex &t = it->second; const Builder &b = t.b;
    const NvnFormat *f = nvn_format(b.format);
    if (!f || f->depth || b.packaged || !t.addr) return;
    int level = 0, layer_base = 0;
    if (const View *v = view ? find_view(view) : nullptr) {
        if (v->set & VS_LEVELS) level = v->base_level;
        if (v->set & VS_LAYERS) layer_base = v->base_layer;
    }
    const int32_t *r = (const int32_t *)(uintptr_t)region;     // x, y, z, width, height, depth (texels)
    int x = r[0], y = r[1], z = r[2], w = r[3], h = r[4], d = std::max(1, r[5]);
    LevelLayout L;
    if (w <= 0 || h <= 0 || !level_layout(b, f, level, L)) return;
    int xb = x / f->bw, yb = y / f->bh, wb = (w + f->bw - 1) / f->bw, hb = (h + f->bh - 1) / f->bh;
    uint64_t row = row_stride ? row_stride : (uint64_t)wb * f->bpb;
    uint64_t img = image_stride ? image_stride : row * hb;
    auto pit = g_pools.find(b.pool);
    uint64_t limit = pit != g_pools.end() ? pit->second.storage + pit->second.size : 0;
    if (b.target == 2) {
        if (limit && t.addr + L.off + bl_level_size_3d(L.wb, L.hb, L.d, f->bpb, L.bh, L.bd) > limit) return;
        bl_write_region((uint8_t *)(uintptr_t)(t.addr + L.off), (const uint8_t *)(uintptr_t)data, xb, yb, z, wb, hb, d, row, img, L.wb, L.hb, f->bpb, L.bh, L.bd);
    } else {
        for (int k = 0; k < d; k++) {     // arrays / cubes: z selects the layer
            uint64_t base = t.addr + (uint64_t)(layer_base + z + k) * L.layer_size + L.off;
            if (limit && base + bl_level_size(L.wb, L.hb, f->bpb, L.bh) > limit) return;
            bl_write_region((uint8_t *)(uintptr_t)base, (const uint8_t *)(uintptr_t)(data + k * img), xb, yb, 0, wb, hb, 1, row, img, L.wb, L.hb, f->bpb, L.bh, 1);
        }
    }
    if (t.gpu) { t.dirty = true; g_tex_gen++; }
    static int logged; if (logged++ < 20) LOG(1, "[nvn] WriteTexels %llx %s %dx%d level %d (%d,%d,%d %dx%dx%d)", (unsigned long long)tex, f->name, b.width, b.height, level, x, y, z, w, h, d);
}
HLE(tex_write_texels) { BOTH; write_texels(c->x[0], c->x[1], c->x[2], c->x[3], 0, 0); }
HLE(tex_write_texels_strided) { BOTH; write_texels(c->x[0], c->x[1], c->x[2], c->x[3], c->x[4], c->x[5]); }
static void upload_texture_impl(Tex &t);
static void upload_texture(Tex &t) { uint64_t t0 = plat_ticks_ns(); upload_texture_impl(t); gpu_prof_add(PROF_TEXUP, plat_ticks_ns() - t0); }
static void upload_texture_impl(Tex &t) {
    const Builder &b = t.b;
    const NvnFormat *f = nvn_format(b.format);
    if (!f || f->depth) return;
    uint64_t src = b.packaged ? b.packaged : t.addr;
    if (!src) return;
    uint64_t limit = UINT64_MAX;
    if (!b.packaged) { auto pit = g_pools.find(b.pool); if (pit == g_pools.end()) return; limit = pit->second.storage + pit->second.size; }
    if (b.target == 2) {   // 3D texture: GOBs are also stacked in depth
        int levels = b.levels > 0 ? b.levels : 1;
        int hb0 = (b.height + f->bh - 1) / f->bh, d0 = b.depth > 0 ? b.depth : 1;
        int bh0 = bl_block_height(hb0), bd0 = bl_block_depth(d0);
        while (bh0 > 1 && bh0 * bd0 > 16) bh0 /= 2;   // a 3D block holds at most 16 GOBs (verified on the game's LUTs/volumes)
        uint64_t off = 0; std::vector<uint8_t> lin;
        for (int l = 0; l < levels; l++) {
            int w = std::max(1, b.width >> l), h = std::max(1, b.height >> l), dz = std::max(1, d0 >> l);
            int wb = (w + f->bw - 1) / f->bw, hb = (h + f->bh - 1) / f->bh;
            int bh = bh0; while (bh > 1 && hb <= (bh / 2) * 8) bh /= 2;
            int bd = bd0; while (bd > 1 && dz <= bd / 2) bd /= 2;
            uint64_t sz = bl_level_size_3d(wb, hb, dz, f->bpb, bh, bd);
            if (src + off + sz > limit) return;
            lin.resize((size_t)wb * hb * dz * f->bpb);
            bl_deswizzle_3d((const uint8_t *)(uintptr_t)(src + off), lin.data(), wb, hb, dz, f->bpb, bh, bd);
            gpu_tex_upload_3d(t.gpu, l, dz, lin.data(), lin.size());
            if (l == 0 && getenv("XRT_DUMP_3D")) {   // raw level 0: deswizzled and as stored (block-linear)
#ifdef _WIN32
                _mkdir("textures");
#else
                mkdir("textures", 0755);
#endif
                char n[128]; snprintf(n, sizeof n, "textures/tex3d_%llx_%dx%dx%d_f%d_bh%d_bd%d.bin", (unsigned long long)src, b.width, b.height, d0, b.format, bh, bd);
                if (FILE *fp = fopen(n, "wb")) { fwrite(lin.data(), 1, lin.size(), fp); fclose(fp); }
                snprintf(n, sizeof n, "textures/tex3d_%llx_raw.bin", (unsigned long long)src);
                if (FILE *fp = fopen(n, "wb")) { fwrite((const void *)(uintptr_t)(src + off), 1, (size_t)sz, fp); fclose(fp); }
            }
            off += sz;
        }
        return;
    }
    int layers = b.target == 2 ? 1 : (b.depth > 0 ? b.depth : 1);
    int levels = b.levels > 0 ? b.levels : 1;
    int hb0 = (b.height + f->bh - 1) / f->bh;
    int bh0 = bl_block_height(hb0);
    // size of one layer = all its levels, aligned to the level-0 block size
    std::vector<uint64_t> level_off(levels), level_size(levels); std::vector<int> level_bh(levels);
    uint64_t layer_size = 0;
    for (int l = 0; l < levels; l++) {
        int w = std::max(1, b.width >> l), h = std::max(1, b.height >> l);
        int wb = (w + f->bw - 1) / f->bw, hb = (h + f->bh - 1) / f->bh;
        int bh = bh0; while (bh > 1 && hb <= (bh / 2) * 8) bh /= 2;
        level_off[l] = layer_size; level_bh[l] = bh; level_size[l] = bl_level_size(wb, hb, f->bpb, bh);
        layer_size += level_size[l];
    }
    if (layers > 1) { uint64_t al = (uint64_t)bh0 * 512; layer_size = (layer_size + al - 1) / al * al; }
    if (src + layer_size * layers > limit) { LOG(2, "[nvn] texture data past its pool, not uploaded"); return; }
    std::vector<uint8_t> lin;
    for (int layer = 0; layer < layers; layer++)
        for (int l = 0; l < levels; l++) {
            int w = std::max(1, b.width >> l), h = std::max(1, b.height >> l);
            int wb = (w + f->bw - 1) / f->bw, hb = (h + f->bh - 1) / f->bh;
            lin.resize((size_t)wb * hb * f->bpb);
            bl_deswizzle((const uint8_t *)(uintptr_t)(src + layer * layer_size + level_off[l]), lin.data(), wb, hb, f->bpb, level_bh[l]);
            gpu_tex_upload(t.gpu, l, layer, lin.data(), lin.size());
        }
}
static void layer_range(const View &v, int &base, int &num, int &level) {
    base = (v.set & VS_LAYERS) ? v.base_layer : 0; num = (v.set & VS_LAYERS) ? v.num_layers : 0;
    level = (v.set & VS_LEVELS) ? v.base_level : 0;
}
static uint64_t g_draws_frame, g_clears_frame;
static bool g_frame_scene;                      // worker: this frame rendered the 3D scene
static std::atomic<int> g_scene_frames{1};      // recent frames with the scene (decays without it; fps_cap)
static GpuDrawState g_st;
// NVNshaderStage (vertex, fragment, geometry, tess control, tess eval) -> GpuStage
static int nvn_stage(int s) { static const int m[5] = {GS_VERTEX, GS_FRAGMENT, GS_GEOMETRY, GS_TESS_CTRL, GS_TESS_EVAL}; return s >= 0 && s < 5 ? m[s] : -1; }
static std::unordered_map<uint64_t, uint64_t> g_code_end;
static uint64_t code_end(uint64_t code) {
    auto it = g_code_end.find(code);
    if (it != g_code_end.end()) return it->second;
    uint64_t limit = 0;
    for (auto &kv : g_pools) if (code >= kv.second.storage && code < kv.second.storage + kv.second.size) limit = kv.second.storage + kv.second.size;
    uint64_t end = code + 0x80;
    if (limit) while (end + 8 <= limit && end < code + (1 << 20)) { end += 8; if (LD64(end - 8) == 0xE2400FFFFF87000FULL) break; }
    return g_code_end[code] = end;
}
static void set_program(uint64_t prog, uint32_t stage_bits) {
    static const int m[6] = {-1, GS_VERTEX, GS_TESS_CTRL, GS_TESS_EVAL, GS_GEOMETRY, GS_FRAGMENT};
    static const uint32_t nvn_bit[6] = {0, 0x1, 0x8, 0x10, 0x4, 0x2};   // NVNshaderStageBits per SPH type
    if (!(stage_bits & 0x1f)) stage_bits = 0x1f;
    g_st.program = prog;
    for (int t = 1; t <= 5; t++) if (stage_bits & nvn_bit[t]) {
        int i = m[t]; g_st.shader[i] = g_st.shader_end[i] = nullptr; g_st.ubo_addr[i][1] = g_st.ubo_size[i][1] = 0; g_xfb_layout[i] = XfbLayout{};
    }
    auto it = g_programs.find(prog);
    if (it == g_programs.end()) return;
    for (const ShaderStage &st : it->second) {
        if (!st.code || LD32(st.code) != 0x12345678) continue;
        uint32_t sph0 = LD32(st.code + 0x30);
        int type = (sph0 >> 10) & 0xf;          // SPH shader type: 1 VS, 2 TCS, 3 TES, 4 GS, 5 PS
        if (type < 1 || type > 5 || !(stage_bits & nvn_bit[type])) continue;
        g_st.shader[m[type]] = (const uint8_t *)(uintptr_t)st.code;
        if (type == 1 || type == 3 || type == 4) {   // stages that can feed transform feedback (VS, TES, GS)
            // layout from the NVN control data: +0xdc varying slots (0x20 + n = generic output n), +0x4dc component
            // count per varying, +0x6dc buffer 0 stride in bytes, +0x6e8 number of varyings. Expanded to GPU attribute
            // words (generic n component c = 0x20 + 4n + c), the numbering Eden's recompiler uses.
            XfbLayout &L = g_xfb_layout[m[type]]; L = XfbLayout{}; L.control = st.control;
            if (st.control && LD32(st.control) == 0x98761234) {
                const uint8_t *slots = (const uint8_t *)(uintptr_t)(st.control + 0xdc);
                const uint8_t *comps = (const uint8_t *)(uintptr_t)(st.control + 0x4dc);
                uint32_t stride = LD32(st.control + 0x6dc), nvar = LD32(st.control + 0x6e8);
                int words = 0;
                if (nvar > 0 && nvar <= 32 && stride > 0 && stride <= 2048) {
                    for (uint32_t v = 0; v < nvar && words < 128; v++) {
                        uint32_t slot = slots[v], nc = comps[v] ? comps[v] : 4;
                        if (slot < 0x20 || slot >= 0x40 || nc > 4) { words = 0; break; }   // only generic outputs for now
                        for (uint32_t c = 0; c < nc && words < 128; c++) L.locs[words++] = (uint8_t)(0x20 + 4 * (slot - 0x20) + c);
                    }
                }
                if (words) { L.count = words; L.stride = (int)stride; }
                static std::set<uint64_t> seen;
                if (nvar && seen.insert(st.control).second && seen.size() <= 32) {
                    char b[300]; int k = 0;
                    for (uint32_t i = 0; i < nvar && i < 32 && k < 260; i++) k += snprintf(b + k, sizeof b - k, "%02x/%d ", slots[i], comps[i]);
                    LOG(1, "[xfb] stage %d shader %llx: %u varyings (slot/components) %s stride %u -> %d words", type, (unsigned long long)st.code, nvar, b, stride, words);
                }
            }
        }
        if (st.const_end > st.const_off && st.const_end - st.const_off <= 0x10000) {
            g_st.ubo_addr[m[type]][1] = st.code + st.const_off; g_st.ubo_size[m[type]][1] = st.const_end - st.const_off;
        }
        g_st.shader_end[m[type]] = (const uint8_t *)(uintptr_t)code_end(st.code);
    }
    // transform feedback captures the last geometry-processing stage
    int last = g_st.shader[GS_GEOMETRY] ? GS_GEOMETRY : g_st.shader[GS_TESS_EVAL] ? GS_TESS_EVAL : GS_VERTEX;
    const XfbLayout &L = g_xfb_layout[last];
    g_st.xfb_count = g_st.shader[last] ? L.count : 0; g_st.xfb_stride = L.stride; g_vs_control = L.control;
    if (g_st.xfb_count) memcpy(g_st.xfb_locs, L.locs, sizeof L.locs);
}
// resolver callbacks for the GPU backend
static GpuTex *res_texture(void *, uint32_t id) {
    const PoolEntry *e = tex_pool_find((int)id);
    GpuTex *g = e ? gpu_tex(e->tex) : nullptr;
    if (!g) {
        static std::set<uint32_t> logged;
        if (logged.insert(id).second) {
            auto t = e ? g_texs.find(e->tex) : g_texs.end();
            if (!e) LOG(1, "[nvn] texture ID %#x is not registered in any texture pool", id);
            else if (t == g_texs.end()) LOG(1, "[nvn] texture ID %#x -> NVNtexture %llx was never initialized through a builder we saw", id, (unsigned long long)e->tex);
            else LOG(1, "[nvn] texture ID %#x: GPU texture creation failed (format %d %dx%dx%d target %d levels %d)", id, t->second.b.format,
                     t->second.b.width, t->second.b.height, t->second.b.depth, t->second.b.target, t->second.b.levels);
        }
    }
    return g;
}
static int res_target(void *, uint32_t id) {
    const PoolEntry *e = tex_pool_find((int)id);
    if (!e) return -1;
    auto t = g_texs.find(e->tex);
    return t == g_texs.end() ? -1 : t->second.b.target;
}
// bit 0: sampled as integer (UI/I formats, stencil), bit 1: depth-stencil texture sampled in stencil mode
static int res_tex_flags(void *, uint32_t id) {
    const PoolEntry *e = tex_pool_find((int)id);
    if (!e) return 0;
    auto t = g_texs.find(e->tex);
    if (t == g_texs.end()) return 0;
    const Builder &b = t->second.b;
    int fmt = (e->view.set & VS_FORMAT) ? e->view.format : b.format;
    const NvnFormat *f = nvn_format(fmt);
    if (f && f->depth && (b.ds_mode == 1 || f->depth == 3)) return 3;
    static const int ints[] = {3, 4, 8, 9, 11, 12, 15, 16, 20, 21, 23, 24, 39, 40, 44, 45, 47, 48, 62, 111, 112, 116, 117, 119, 120};
    for (int i : ints) if (fmt == i) return 1;
    return 0;
}
static GpuTex *res_image(void *, uint32_t id, int *level) {
    const PoolEntry *e = nullptr;
    for (uint64_t pool : {g_cur_tex_pool, g_last_tex_pool}) {
        auto p = g_img_pools.find(pool);
        if (p == g_img_pools.end()) continue;
        auto it = p->second.find((int)id);
        if (it != p->second.end()) { e = &it->second; break; }
    }
    if (!e) for (auto &p : g_img_pools) { auto it = p.second.find((int)id); if (it != p.second.end()) { e = &it->second; break; } }
    if (!e) return nullptr;
    *level = (e->view.set & VS_LEVELS) ? e->view.base_level : 0;
    return gpu_tex(e->tex, true);    // as_rt: written by shaders, no upload needed... but keep contents if already created
}
// packed NVN swizzle (r | g<<8 | b<<16 | a<<24) of a texture ID: view swizzle overrides the texture's own
static uint32_t res_tex_swizzle(void *, uint32_t id) {
    const PoolEntry *e = tex_pool_find((int)id);
    if (!e) return 0x05040302;
    const uint8_t *s = nullptr;
    if (e->view.set & VS_SWIZZLE) s = e->view.swz;
    else { auto t = g_texs.find(e->tex); if (t != g_texs.end()) s = t->second.b.swz; }
    return s ? (uint32_t)s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16 | (uint32_t)s[3] << 24 : 0x05040302;
}
static uint64_t res_tex_addr(void *, uint32_t id) {
    const PoolEntry *e = tex_pool_find((int)id);
    if (!e) return 0;
    auto t = g_texs.find(e->tex);
    return t == g_texs.end() ? 0 : t->second.addr;
}
static bool res_tex_buffer(void *, uint32_t id, uint64_t *addr, uint64_t *size, int *fmt) {
    const PoolEntry *e = tex_pool_find((int)id);
    if (!e) return false;
    auto t = g_texs.find(e->tex);
    if (t == g_texs.end() || t->second.b.target != 10) return false;     // NVN_TEXTURE_TARGET_BUFFER
    const Builder &b = t->second.b;
    *fmt = (e->view.set & VS_FORMAT) ? e->view.format : b.format;
    const NvnFormat *f = nvn_format(*fmt);
    *addr = t->second.addr;
    *size = f ? (uint64_t)b.width * (uint64_t)f->bpb : t->second.size;
    return *addr != 0;
}
static bool res_sampler(void *, uint32_t id, GpuSampler *out) {
    for (uint64_t pool : {g_cur_smp_pool, g_last_smp_pool}) {
        auto p = g_sampler_pools.find(pool);
        if (p == g_sampler_pools.end()) continue;
        auto it = p->second.find(id);
        if (it != p->second.end()) { *out = it->second; return true; }
    }
    return false;
}
static void exec(const OpList &ops, bool log, int depth = 0) {
    for (const Op &o : ops) {
        nl_yield();
        switch (o.kind) {
        case OP_RT: {
            // pass boundary: hand what is recorded to the GPU every XRT_KICK_DRAWS draws (default 250, 0 = only at present)
            { static long every = -1; if (every < 0) { const char *e = getenv("XRT_KICK_DRAWS"); every = e ? atol(e) : 250; }
              static uint64_t at; if (g_draws_frame < at) at = 0;
              if (every > 0 && g_render > 0 && g_draws_frame - at >= (uint64_t)every) { at = g_draws_frame; gpu_kick(); } }
            const RtData &r = *o.rt;
            g_rt.n = o.n;
            for (int i = 0; i < 8; i++) { g_rt.color[i] = i < o.n ? r.tex[i] : 0; g_rt.cview[i] = r.view[i]; }
            g_rt.depth = r.tex[8]; g_rt.dview = r.view[8];
            if (!g_frame_scene && o.n > 0) {   // a 3D scene pass this frame (see fps_cap)
                auto ti = g_texs.find(r.tex[0]);
                int sw = g_res_w ? g_res_w : 960, sh = g_res_h ? g_res_h : 544;
                if (ti != g_texs.end() && ti->second.b.width == sw && ti->second.b.height == sh) g_frame_scene = true;
            }
            g_st.num_colors = o.n;
            for (int i = 0; i < 8; i++) {
                g_st.color[i] = g_rt.color[i] ? gpu_tex(g_rt.color[i], true) : nullptr;
                int base, num, level; layer_range(g_rt.cview[i], base, num, level); g_st.color_layer[i] = base; g_st.color_level[i] = level;
            }
            g_st.depth = g_rt.depth ? gpu_tex(g_rt.depth, true) : nullptr;
            { int base, num, level; layer_range(g_rt.dview, base, num, level); g_st.depth_layer = base; g_st.depth_level = level; }
            if (log) {
                std::string s;
                for (int i = 0; i < o.n; i++) s += " " + tex_desc(r.tex[i]);
                LOG(0, "[nvnlog]   SetRenderTargets%s | depth %s", s.c_str(), tex_desc(r.tex[8]).c_str());
            }
            break;
        }
        case OP_CLEAR_COLOR: {
            int idx = o.i[0];
            uint64_t t = idx >= 0 && idx < 8 ? g_rt.color[idx] : 0;
            if (log) LOG(0, "[nvnlog]   ClearColor[%d] (%.3f %.3f %.3f %.3f) mask %x -> %s", idx, o.f[0], o.f[1], o.f[2], o.f[3], o.i[1], tex_desc(t).c_str());
            g_clears_frame++;
            if (!t || (o.i[1] & 0xf) == 0) break;
            int base, num, level; layer_range(g_rt.cview[idx], base, num, level);
            gpu_clear_color(gpu_tex(t), base, num, level, o.f);
            break;
        }
        case OP_CLEAR_DS: {
            if (log) LOG(0, "[nvnlog]   ClearDepthStencil depth=%.3f dmask=%d stencil=%d smask=%x -> %s", o.f[0], o.i[0], o.i[1], o.i[2], tex_desc(g_rt.depth).c_str());
            g_clears_frame++;
            if (!g_rt.depth) break;
            int base, num, level; layer_range(g_rt.dview, base, num, level);
            gpu_clear_depth_stencil(gpu_tex(g_rt.depth), base, num, level, o.i[0] != 0, o.f[0], o.i[2] != 0, o.i[1]);
            break;
        }
        case OP_DRAW: {
            g_draws_frame++;
            GpuDraw d; d.prim = o.i[0]; d.indexed = o.n == 1; d.index_type = o.i[1]; d.count = (uint32_t)o.i[2]; d.first = (uint32_t)o.i[3];
            d.index_addr = o.u[0]; d.base_vertex = o.i[5]; d.base_instance = (uint32_t)o.i[6]; d.instances = (uint32_t)(o.i[7] > 0 ? o.i[7] : 1);
            if (log) LOG(0, "[nvnlog]   Draw%s prim=%d count=%u inst=%u", d.indexed ? "Elements" : "Arrays", d.prim, d.count, d.instances);
            // rasterizer-discard draws are vertex shaders used as compute (animation / skinning data written to
            // storage buffers): they must run, only rasterization is off
            g_st.rast_discard = g_rast_discard;
            if (g_xfb_watch && !g_rast_discard) {   // which draws consume the transform-feedback output (not emulated yet)?
                auto hit = [](uint64_t a, uint64_t sz) { return a && a <= g_xfb_watch + g_xfb_watch_size && g_xfb_watch < a + (sz ? sz : 1); };
                const char *via = nullptr; int st = -1, idx = -1;
                for (int i = 0; i < 16 && !via; i++) if (hit(g_st.vb_addr[i], g_st.vb_size[i])) { via = "vertex buffer"; idx = i; }
                for (int k = 0; k < GS_COUNT && !via; k++) for (int b = 0; b < 18 && !via; b++) if (hit(g_st.ubo_addr[k][b], g_st.ubo_size[k][b])) { via = "uniform buffer"; st = k; idx = b; }
                for (int k = 0; k < GS_COUNT && !via; k++) for (int b = 0; b < 16 && !via; b++) if (hit(g_st.ssbo_addr[k][b], g_st.ssbo_size[k][b])) { via = "storage buffer"; st = k; idx = b; }
                static std::set<uint64_t> seen_prog;
                if (via && seen_prog.insert(g_st.program).second && seen_prog.size() < 40) {
                    const float *f = (const float *)(uintptr_t)g_xfb_watch;
                    LOG(1, "[xfb] prog %llx reads the XFB buffer via %s stage %d #%d (prim %d count %u inst %u); contents %g %g %g %g",
                        (unsigned long long)g_st.program, via, st, idx, d.prim, d.count, d.instances, f[0], f[1], f[2], f[3]);
                }
            }
            bool skip_this = false;
            if (g_rast_discard) {
                // diagnostics: every distinct discard program once, with what it binds
                static std::set<uint64_t> seen_d;
                if (seen_d.insert(g_st.program).second && seen_d.size() <= 64) {
                    char sb[512]; int n = 0; sb[0] = 0;
                    for (int k = 0; k < GS_COUNT; k++) for (int b = 0; b < 16; b++) if (g_st.ssbo_addr[k][b] && n < 480)
                        n += snprintf(sb + n, sizeof sb - n, " ssbo%d.%d=%llx+%llx", k, b, (unsigned long long)g_st.ssbo_addr[k][b], (unsigned long long)g_st.ssbo_size[k][b]);
                    LOG(1, "[discard] prog %llx vs %p fs %p prim %d count %u inst %u xfb %d%s", (unsigned long long)g_st.program, (const void *)g_st.shader[GS_VERTEX],
                        (const void *)g_st.shader[GS_FRAGMENT], d.prim, d.count, d.instances, g_xfb_active ? 1 : 0, sb);
                }
                // XRT_SKIP_DISCARD=1 skips all of them; XRT_SKIP_DISCARD=<hex prog>,<hex prog> only those programs
                static std::vector<uint64_t> skip_list; static int skip_all = -1;
                if (skip_all < 0) {
                    const char *e = getenv("XRT_SKIP_DISCARD"); skip_all = 0;
                    if (e && !strcmp(e, "1")) skip_all = 1;
                    else if (e) for (const char *q = e; *q;) { char *end; uint64_t v = strtoull(q, &end, 16); if (end == q) break; skip_list.push_back(v); q = *end ? end + 1 : end; }
                }
                skip_this = skip_all == 1;
                for (uint64_t v : skip_list) if (v == g_st.program) skip_this = true;
            }
            if (g_st.xfb_active && !g_st.xfb_count && g_vs_control) {   // transform feedback on, but no layout recognised: dump the control data
                static std::set<uint64_t> dumped;
                if (dumped.insert(g_vs_control).second && dumped.size() <= 16) {
                    char name[96]; snprintf(name, sizeof name, "xfb_ctrl_%llx.bin", (unsigned long long)g_vs_control);
                    if (FILE *f = fopen(name, "wb")) { fwrite((void *)(uintptr_t)g_vs_control, 1, 0x1000, f); fclose(f); }
                    LOG(1, "[xfb] no capture layout found for vertex shader control %llx (prog %llx): dumped to %s", (unsigned long long)g_vs_control, (unsigned long long)g_st.program, name);
                }
            }
            if (g_render && !skip_this) { gpu_draw(g_st, d); if (g_st.xfb_active) g_st.xfb_fresh = false; }
            break;
        }
        case OP_PROGRAM: set_program(o.u[0], (uint32_t)o.u[1]); break;
        case OP_VATTR: {
            g_st.num_attribs = o.n;
            for (int i = 0; i < o.n; i++) { g_st.attribs[i].format = (int)(*o.words)[3 * i]; g_st.attribs[i].offset = (*o.words)[3 * i + 1]; g_st.attribs[i].stream = (int)(*o.words)[3 * i + 2]; }
            break;
        }
        case OP_VSTREAM: {
            g_st.num_streams = o.n;
            for (int i = 0; i < o.n; i++) { g_st.streams[i].stride = (*o.words)[2 * i]; g_st.streams[i].divisor = (*o.words)[2 * i + 1]; }
            break;
        }
        case OP_VB: if (o.i[0] >= 0 && o.i[0] < 16) { g_st.vb_addr[o.i[0]] = o.u[0]; g_st.vb_size[o.i[0]] = o.u[1]; } break;
        case OP_UBO: {
            // NVN uniform buffer index i is hardware constant buffer bank i + 3 (banks 0-2 belong to the
            // driver; bank 2 holds the bound texture handles) - confirmed by the shaders' bank usage.
            int st = nvn_stage(o.i[0]), bank = o.i[1] + 3;
            if (st >= 0 && o.i[1] >= 0 && bank < 18) { g_st.ubo_addr[st][bank] = o.u[0]; g_st.ubo_size[st][bank] = o.u[1]; }
            break;
        }
        case OP_TEX_POOL: if (g_cur_tex_pool != o.u[0]) g_tex_gen++; g_cur_tex_pool = o.u[0]; break;
        case OP_SMP_POOL: if (g_cur_smp_pool != o.u[0]) g_tex_gen++; g_cur_smp_pool = o.u[0]; break;
        case OP_IMAGE: { int st = nvn_stage(o.i[0]); if (st >= 0 && o.i[1] >= 0 && o.i[1] < 8) g_st.bound_img[st][o.i[1]] = o.u[0]; break; }
        case OP_TEX: { int st = nvn_stage(o.i[0]); if (st >= 0 && o.i[1] >= 0 && o.i[1] < 32) g_st.bound_tex[st][o.i[1]] = o.u[0]; break; }
        case OP_BLEND: if (o.i[0] >= 0 && o.i[0] < 8) g_st.blend[o.i[0]] = GpuBlend{o.i[1], o.i[2], o.i[3], o.i[4], o.i[5], o.i[6]}; break;
        case OP_COLOR: g_st.blend_enable = (uint8_t)o.i[0]; g_st.logic_op = o.i[1]; g_st.alpha_func = o.i[2]; break;
        case OP_POLY_OFFSET: for (int i = 0; i < 3; i++) g_st.poly_offset[i] = o.f[i]; break;
        case OP_ALPHA_REF: g_st.alpha_ref = o.f[0]; break;
        case OP_CMASK: for (int i = 0; i < 8; i++) g_st.channel_mask[i] = (uint8_t)o.i[i]; break;
        case OP_DS: g_st.depth_test = o.i[0]; g_st.depth_write = o.i[1]; g_st.depth_func = o.i[2]; g_st.stencil_test = o.i[3]; break;
        case OP_STENCIL:
            g_st.stencil_func[0] = o.i[0]; g_st.stencil_func[1] = o.i[1];
            for (int k = 0; k < 3; k++) { g_st.stencil_op[0][k] = o.i[2 + k]; g_st.stencil_op[1][k] = o.i[5 + k]; }
            break;
        case OP_STENCIL_DYN:
            for (int f = 0; f < 2; f++) if (o.i[1] & (1 << f)) {
                uint8_t v = (uint8_t)o.i[2];
                if (o.i[0] == 0) g_st.stencil_ref[f] = v; else if (o.i[0] == 1) g_st.stencil_vmask[f] = v; else g_st.stencil_wmask[f] = v;
            }
            break;
        case OP_POLY: g_st.cull_face = o.i[0]; g_st.front_face = o.i[1]; g_st.polygon_mode = o.i[2]; g_st.poly_offset_enables = o.i[3]; break;
        case OP_VIEWPORT: for (int i = 0; i < 4; i++) g_st.viewport[i] = o.i[i]; break;
        case OP_SCISSOR: for (int i = 0; i < 4; i++) g_st.scissor[i] = o.i[i]; break;
        case OP_SSBO: { int st = nvn_stage(o.i[0]); if (st >= 0 && o.i[1] >= 0 && o.i[1] < 16) { g_st.ssbo_addr[st][o.i[1]] = o.u[0]; g_st.ssbo_size[st][o.i[1]] = o.u[1]; } break; }
        case OP_CLEAR_COLOR_INT: {
            int idx = o.i[0];
            uint64_t t = idx >= 0 && idx < 8 ? g_rt.color[idx] : 0;
            g_clears_frame++;
            if (!t || (o.i[1] & 0xf) == 0) break;
            int base, num, level; layer_range(g_rt.cview[idx], base, num, level);
            uint32_t v[4]; std::memcpy(v, &o.i[2], 16);
            gpu_clear_color_int(gpu_tex(t, true), base, num, level, v);
            break;
        }
        case OP_RAST_DISCARD: g_rast_discard = o.i[0] != 0; break;
        case OP_XFB: {
            static int n;
            static const char *what[4] = {"BindBuffer", "Begin", "End", "DrawTransformFeedback"};
            if (n++ < 60) LOG(1, "[xfb] %s i=%d a=%llx b=%llx prog %llx", what[o.i[0] & 3], o.i[1], (unsigned long long)o.u[0], (unsigned long long)o.u[1], (unsigned long long)g_st.program);
            if (o.i[0] == 0 && o.u[0]) { g_xfb_watch = o.u[0]; g_xfb_watch_size = o.u[1] ? o.u[1] : 16; }
            if (o.i[0] == 0 && o.i[1] == 0) { g_st.xfb_addr = o.u[0]; g_st.xfb_size = o.u[1]; }
            if (o.i[0] == 1) { g_xfb_active = true; g_st.xfb_active = !getenv("XRT_NO_XFB"); g_st.xfb_control = o.u[0]; g_st.xfb_fresh = true; }
            if (o.i[0] == 2) { g_xfb_active = false; g_st.xfb_active = false; }
            if (o.i[0] == 3 && g_render > 0 && !getenv("XRT_NO_XFB")) {
                // draw what an earlier transform-feedback pass wrote (GPU particle ping-pong): the count is in its control buffer
                GpuDraw d; d.prim = o.i[1]; d.count = 0; d.instances = 1; d.xfb_count_addr = o.u[0];
                g_st.rast_discard = g_rast_discard;
                gpu_draw(g_st, d);
                if (g_st.xfb_active) g_st.xfb_fresh = false;
            }
            break;
        }
        case OP_COUNTER: {
            static std::set<int> seen;
            if (seen.insert(o.i[0]).second) LOG(1, "[nvn] ReportCounter type %d -> %llx", o.i[0], (unsigned long long)o.u[0]);
            // GPU timestamps: the game measures GPU time between reports to pick its dynamic resolution. Our
            // replay runs on the host GPU at present, so the CPU clock at record time would charge our recording
            // overhead as GPU time and the game would drop to its lowest resolution. Report a per-frame base
            // plus one tick per report (a GPU with headroom); XRT_REAL_GPU_TIME=1 restores the old behaviour.
            static int real_time = -1; if (real_time < 0) real_time = getenv("XRT_REAL_GPU_TIME") ? 1 : 0;
            uint64_t ticks = real_time ? plat_ticks_ns() * 12 / 625 : g_ts_base + (++g_ts_count);   // 19.2 MHz Switch ticks
            uint64_t v[2] = {o.i[0] == 0 || o.i[0] == 16 ? ticks : 0x100000ULL, ticks};
            if (o.u[0]) memcpy((void *)(uintptr_t)o.u[0], v, 16);
            break;
        }
        case OP_BARRIER: gpu_barrier((uint32_t)o.u[0]); break;
        case OP_CLEAR_BUFFER: gpu_fill_buffer(o.u[0], o.u[1], (uint32_t)o.i[0]); break;
        case OP_DEPTH_RANGE: g_st.depth_range[0] = o.f[0]; g_st.depth_range[1] = o.f[1]; break;
        case OP_BLEND_COLOR: for (int i = 0; i < 4; i++) g_st.blend_color[i] = o.f[i]; break;
        case OP_CALL:
            if (depth < 8) for (auto &l : o.calls) { if (log) LOG(0, "[nvnlog]  call (%zu ops)", l->size()); exec(*l, log, depth + 1); }
            break;
        }
    }
}
// ---------------------------------------------------------------- render worker
// The game's queue calls become jobs, run in order by one worker thread that owns the Vulkan backend: SUBMIT (mirror
// sync + translation of the submitted command lists), PRESENT, FENCE (nvnQueueFenceSync: note the GPU frame serial),
// WAIT (nvnSyncWait: run everything up to the fence, then wait for its GPU work) and FINISH (nvnQueueFinish). The
// game blocks only in WAIT / FINISH, and at a present when the worker is still on the previous frame's present (at
// most one frame queued, like the GPU's own frame in flight). Memory semantics are those of the real GPU: the
// translation reads the game's buffers when it runs, which is no later than the game's wait on that frame's fence.
// XRT_RENDER_WORKER=0: run every job on the calling game thread at once (the old behaviour).
enum JobKind { J_SUBMIT, J_PRESENT, J_FENCE, J_WAIT, J_FINISH };
struct Job { JobKind kind; uint64_t seq = 0, a = 0; void *snap = nullptr; std::vector<std::shared_ptr<OpList>> lists; };
static std::mutex g_qm;
static std::condition_variable g_q_cv, g_done_cv;
static std::deque<Job> g_q;
static uint64_t g_q_seq;                               // last job enqueued (g_qm)
static std::atomic<uint64_t> g_q_done{0};              // last job finished
static std::unordered_map<uint64_t, uint64_t> g_sync_fence;   // NVNsync -> seq of its FENCE job (g_qm)
static const int FENCE_RING = 4096;
static struct { uint64_t seq, serial; } g_fence_serial[FENCE_RING];   // FENCE job seq -> GPU frame serial (worker)
static bool worker_on() { static int w = -1; if (w < 0) { const char *e = getenv("XRT_RENDER_WORKER"); w = e ? (atoi(e) != 0) : 1; if (w) LOG(1, "[nvn] render worker thread on (XRT_RENDER_WORKER=0 to translate on the game thread)"); } return w == 1; }

static void present_job(uint64_t tex);
static bool wait_gpu() { static int w = -1; if (w < 0) { const char *e = getenv("XRT_WAIT_GPU"); w = e ? atoi(e) != 0 : 0; } return w == 1; }
static void run_job(Job &j) {   // object lock held
    if (g_render < 0) {
        g_render = getenv("XRT_NO_DRAW") ? 0 : 1;
        const char *fy = getenv("XRT_FLIP_Y");
        g_st.lower_left = fy ? atoi(fy) != 0 : g_origin_mode == 1;
        gpu_set_resolver(GpuResolver{nullptr, res_texture, res_target, res_sampler, res_tex_flags, res_image, res_tex_swizzle, res_tex_buffer, res_tex_addr});
    }
    switch (j.kind) {
    case J_SUBMIT: {
        bool log = log_frame();
        uint64_t t0 = plat_ticks_ns();
        if (g_render > 0) gpu_mirrors_sync(j.a, j.snap); j.snap = nullptr;
        for (auto &l : j.lists) {
            if (log) LOG(0, "[nvnlog] frame %llu submit (%zu ops)", (unsigned long long)g_frames.load(), l->size());
            exec(*l, log);
        }
        gpu_prof_add(PROF_EXEC, plat_ticks_ns() - t0);
        break;
    }
    case J_PRESENT: present_job(j.a); break;
    case J_FENCE: g_fence_serial[j.seq % FENCE_RING] = {j.seq, gpu_sync_point()}; break;
    case J_WAIT:
        // XRT_WAIT_GPU=0: a sync wait only waits for the translation of the work before the fence, not for the GPU to
        // run it: what the GPU reads from guest memory was copied into staging / VRAM mirrors while translating.
        if (sync_flush_enabled() && g_render > 0 && wait_gpu()) {
            auto &f = g_fence_serial[j.a % FENCE_RING];
            if (j.a && f.seq == j.a) gpu_sync_wait(f.serial); else gpu_flush();   // (a sync never fenced: run what is recorded)
        }
        break;
    case J_FINISH: if (sync_flush_enabled() && g_render > 0) gpu_finish(); break;
    }
}
static void worker_main() {
    xrt_sampler_register_thread("render worker (NVN->Vulkan)");
    for (;;) {
        Job j;
        { std::unique_lock<std::mutex> l(g_qm); g_q_cv.wait(l, [] { return !g_q.empty(); }); j = std::move(g_q.front()); g_q.pop_front(); }
        g_nl.lock();
        run_job(j);
        g_nl.unlock();
        j.lists.clear();   // (drop the command lists outside the lock)
        { std::lock_guard<std::mutex> l(g_qm); g_q_done.store(j.seq); }
        g_done_cv.notify_all();
    }
}
static void wait_done(uint64_t seq) {
    if (g_q_done.load() >= seq) return;
    uint64_t t0 = plat_ticks_ns();
    { std::unique_lock<std::mutex> l(g_qm); g_done_cv.wait(l, [seq] { return g_q_done.load() >= seq; }); }
    g_worker_wait_ns += plat_ticks_ns() - t0; g_worker_waits++;
}
// run (or queue) a job; returns its seq. wait: block until it has run.
static uint64_t dispatch(Job &&j, bool wait) {
    if (!worker_on()) {
        std::lock_guard<std::mutex> l(g_qm); j.seq = ++g_q_seq;   // (g_qm also serializes inline jobs from several threads)
        NLOCK; run_job(j); g_q_done.store(j.seq);
        return j.seq;
    }
    static std::once_flag started;
    std::call_once(started, [] { std::thread(worker_main).detach(); });
    uint64_t seq;
    { std::lock_guard<std::mutex> l(g_qm); seq = j.seq = ++g_q_seq; g_q.push_back(std::move(j)); }
    g_q_cv.notify_one();
    if (wait) wait_done(seq);
    return seq;
}

static std::deque<std::pair<uint64_t, uint64_t>> g_fence_order;   // (fence job seq, the fence job before it) (g_qm)
static uint64_t g_last_fence;                                        // (g_qm)
static bool frame_lag() { static int f = -1; if (f < 0) { f = getenv("XRT_FRAME_LAG") && atoi(getenv("XRT_FRAME_LAG")) && worker_on() ? 1 : 0; if (f) { LOG(1, "[nvn] frame lag: the game runs up to 2 frames ahead (pool writes snapshotted at submit)"); gpu_set_snapshots(true); } } return f == 1; }
HLE(sync_wait) {
    g_n_sync_wait++; order_log("SyncWait", c);
    { static int n; if (n++ < 6) LOG(1, "[nvn] SyncWait(%llx, timeout %lld) lr %llx", (unsigned long long)c->x[0], (long long)c->x[1], (unsigned long long)c->x[30]); }
    Job j; j.kind = J_WAIT;
    { std::lock_guard<std::mutex> l(g_qm); auto it = g_sync_fence.find(c->x[0]); if (it != g_sync_fence.end()) j.a = it->second; }
    if (frame_lag() && j.a) {
        // XRT_FRAME_LAG=1: the game may run one frame further ahead. A wait on frame N's fence only waits until the
        // worker has translated frame N-1 (its fence job ran); GPU completion is left to the frame slots.
        uint64_t before = 0;
        { std::lock_guard<std::mutex> l(g_qm); for (auto &fp : g_fence_order) if (fp.first == j.a) { before = fp.second; break; } }
        if (before) wait_done(before);
        RET(0); return;
    }
    dispatch(std::move(j), true);
    RET(0); }   // NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED (the GPU work it stands for is done)
HLE(queue_fence_sync) {
    g_n_queue_fence++; order_log("QueueFenceSync", c);
    Job j; j.kind = J_FENCE;
    uint64_t seq = dispatch(std::move(j), false);
    { std::lock_guard<std::mutex> l(g_qm); g_sync_fence[c->x[1]] = seq;
      g_fence_order.push_back({seq, g_last_fence}); g_last_fence = seq; if (g_fence_order.size() > 64) g_fence_order.pop_front(); }
    { static int n; if (n++ < 6) LOG(1, "[nvn] QueueFenceSync(queue %llx, sync %llx, cond %d, flags %d) lr %llx", (unsigned long long)c->x[0], (unsigned long long)c->x[1], (int)c->x[2], (int)c->x[3], (unsigned long long)c->x[30]); }
}
HLE(queue_finish) { g_n_finish++; Job j; j.kind = J_FINISH; dispatch(std::move(j), true); }
HLE(queue_submit) {      // (queue, int count, const NVNcommandHandle* handles)
    order_log("QueueSubmit", c);
    Job j; j.kind = J_SUBMIT;
    if (frame_lag()) j.snap = gpu_mirrors_snapshot();   // (the game may rewrite this frame's data before the worker gets to it)
    else j.a = gpu_mirrors_prescan();   // (the CPU writes this frame's GPU work needs are in memory now: scan while the worker is busy)
    {
        RLOCK;
        for (int i = 0; i < (int)c->x[1]; i++) {
            uint64_t hd = LD64(c->x[2] + 8 * i);
            auto it = g_handles.find(hd);
            if (it != g_handles.end()) { j.lists.push_back(it->second); handle_used(hd); }
            else if (hd && g_handle_misses++ < 8) LOG(1, "[nvn] submit of a released command handle %llu", (unsigned long long)hd);
        }
    }
    dispatch(std::move(j), false);
}
extern "C" void xrt_mark_render_thread(void);
extern "C" void hle_prof_report(double frames);
// The game's frame limiter (main+0xf63174, 1.0.0) busy-waits on the system tick until frame_period x interval x 0.997
// has passed, interval being an int behind the pointer at main+0x1791d48 (2 = 30 fps). XRT_FRAME_INTERVAL=n forces
// it every frame (1 = 60 fps limit). Game-version specific: checked against the code bytes first.
static void force_frame_interval() {
    static int want = -2; static int *slot;
    if (want == -2) {
        const char *e = getenv("XRT_FRAME_INTERVAL"); want = e ? atoi(e) : -1;
        if (want >= 0) {
            const uint32_t *code = (const uint32_t *)(g_img.base + 0xf63218);   // ldr w23, [x21]
            if (code[0] != 0xb94002b7) { LOG(0, "[fps] frame limiter not where expected (%08x) - XRT_FRAME_INTERVAL ignored", code[0]); want = -1; }
            else { slot = *(int **)(g_img.base + 0x1791d48); LOG(0, "[fps] frame limiter interval at %p (was %d), forcing %d", (void *)slot, slot ? *slot : -1, want); }
        }
    }
    if (want >= 0 && slot) *slot = want;
}
static uint64_t g_game_frames;   // presents issued by the game (its render thread)
static uint64_t handle_frame() { return g_game_frames; }
static void present_job(uint64_t tex) {   // worker: the frame is complete
    g_ts_base = plat_ticks_ns() * 12 / 625; g_ts_count = 0;
    log_frame();
    uint64_t n = ++g_frames;
    if ((int64_t)n > g_nvnlog_from && (int64_t)n <= g_nvnlog_from + g_nvnlog) LOG(0, "[nvnlog] frame %llu present %s (%llu draws, %llu clears)", (unsigned long long)n, tex_desc(tex).c_str(),
                                    (unsigned long long)g_draws_frame, (unsigned long long)g_clears_frame);
    gpu_present(tex ? gpu_tex(tex) : nullptr);
    { int v = g_scene_frames.load(); g_scene_frames.store(g_frame_scene ? 8 : (v > 0 ? v - 1 : 0)); g_frame_scene = false; }
    static uint64_t t0 = plat_ticks_ns(), draws = 0, tw = plat_ticks_ns();
    draws += g_draws_frame; g_draws_frame = g_clears_frame = 0;
    if (n % 300 == 0) {
        uint64_t now = plat_ticks_ns();
        gpu_prof_report(300, (now - tw) / 1e9); tw = now; nvn_sync_stats(300); hle_prof_report(300);
        double s = (now - t0) / 1e9;
        LOG(1, "[nvn] %llu frames presented (%.1f fps avg, %.0f draws/frame)", (unsigned long long)n, n / (s > 0 ? s : 1), draws / 300.0);
        draws = 0;
    }
}
// Frame cap (milestone M5): game/patches/hfr_2.2.0.pchtxt turns the game's own 60 fps busy-wait off and widens its
// frame-time range (game speed follows the measured frame time down to 1/480 s); the cap is applied here, on the game's
// render thread before it hands the frame over. XRT_FPS_CAP=n (default 120), 0 = uncapped.
static void fps_cap() {
    static uint64_t next;
    // (xc3.ini / TAB menu, XRT_FPS_CAP / XRT_MENU_FPS override; read every frame so changes apply at once)
    const XrtSettings &st = settings();
    int64_t period = st.fps_cap > 0 ? 1000000000LL / st.fps_cap : 0;
    // Menus and other screens without the 3D scene step their animations per frame (not by frame time): they keep
    // the 60 fps pace (menu_fps, default 60; 0 = like the scene).
    int64_t menu_period = st.menu_fps > 0 ? 1000000000LL / st.menu_fps : 0;
    { static int logged; if (!logged++) LOG(1, "[fps] frame cap %d, menus %d (0 = off)", st.fps_cap, st.menu_fps); }
    int64_t per = period;
    if (int fg = gpu_fg_cap_hz(); fg > 0) { int64_t fp = 1000000000LL / fg; if (per <= 0 || per < fp) per = fp; }   // (frame generation: half the refresh rate)
    if (menu_period && g_scene_frames.load(std::memory_order_relaxed) == 0 && (per <= 0 || per < menu_period)) per = menu_period;
    // The game's own 60 fps mode flag (the 60fps patch: 60 in the field, 30 elsewhere): its menus count frames (cursor
    // repeat ...) at 30 fps there. XRT_GAME30=1: keep those screens at 30 fps (frame generation doubles them).
    {
        static int game30 = -1; static int last = -1; static uint64_t t0 = plat_ticks_ns();
        if (game30 < 0) { const char *e = getenv("XRT_GAME30"); game30 = e ? atoi(e) : 0; }
        const uint8_t *flagp = *(const uint8_t *const *)(g_img.base + 0x1b655d8);
        int fast = flagp ? *flagp : 1;
        if (fast != last) {
            LOG(1, "[fps] game 60 fps mode %s at %.1f s (frame %llu, scene %d)", fast ? "on" : "off", (plat_ticks_ns() - t0) / 1e9,
                (unsigned long long)g_game_frames, g_scene_frames.load());
            last = fast;
        }
        if (game30 && !fast && (per <= 0 || per < 1000000000LL / 30)) per = 1000000000LL / 30;
    }
    if (per <= 0) { next = 0; return; }
    uint64_t now = plat_ticks_ns();
    if (!next || now > next + 2 * (uint64_t)per) { next = now + per; return; }   // (first frame, or far behind: restart)
    if (now < next) plat_sleep_until_ns(next);
    next += per;
}
// Fixed-step timers (menus and other UI): the 60 fps patch steps two per-frame timers by a constant that is right at
// 60 fps (1/120 per UI tick, 1/60 for the other one; 1/60 and the game's 30 fps value outside the 60 fps mode). Above
// 60 fps those ran faster with the frame rate. hfr_2.2.0.pchtxt puts hvc #1 / #2 there: they are scaled to the real
// frame time of the game's last frames here. XRT_UI_DT=0: the 60 fps constants as before.
extern "C" void findctr_frame(void);
static std::atomic<float> g_game_dt{1.0f / 60};
static void note_game_frame() {
    static uint64_t last; static float hist[4] = {1.0f / 60, 1.0f / 60, 1.0f / 60, 1.0f / 60}; static int k;
    uint64_t now = plat_ticks_ns();
    if (last) {
        float dt = (float)((now - last) / 1e9);
        if (dt < 1.0f / 480) dt = 1.0f / 480;
        if (dt > 1.0f / 20) dt = 1.0f / 20;   // (a hitch: not a 20x jump of the menu)
        hist[k++ & 3] = dt;
        g_game_dt.store((hist[0] + hist[1] + hist[2] + hist[3]) / 4, std::memory_order_relaxed);
    }
    last = now;
}
// Menu cursor repeat: main+0xe6b3e0 (the pad update) adds frame info +4 (a fixed 1/30 s) to a hold timer per button
// every frame and compares those with its repeat delays - at 120 fps the cursor ran 4x too fast (found with
// XRT_FIND_REPEAT). It gets a copy of the frame info with the real frame time there. XRT_MENU_DT=0: off.
extern "C" int pad_hold_dt_hook(Ctx *c) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("XRT_MENU_DT"); on = e ? atoi(e) : 1; LOG(1, "[fps] menu cursor repeat follows the frame time: %s", on ? "on" : "off (XRT_MENU_DT=0)"); }
    if (!on || !c->x[1]) return 0;
    static thread_local uint64_t info_q[0x60 / 8]; uint8_t *info = (uint8_t *)info_q;
    std::memcpy(info, (const void *)(uintptr_t)c->x[1], sizeof info);
    float dt = g_game_dt.load(std::memory_order_relaxed);
    std::memcpy(info + 4, &dt, 4);
    c->x[1] = (uint64_t)(uintptr_t)info;
    return 0;   // (the function runs, with the copy)
}
extern "C" void xrt_hvc(Ctx *c, uint64_t imm) {
    static int mode = -1;
    if (mode < 0) { const char *e = getenv("XRT_UI_DT"); mode = e ? atoi(e) : 1; LOG(1, "[fps] fixed-step timers follow the frame time: %s", mode ? "on" : "off (XRT_UI_DT=0)"); }
    // the 60 fps mode flag of the game (latched once a frame; the 60fps patch reads the same byte)
    const uint8_t *flagp = *(const uint8_t *const *)(g_img.base + 0x1b655d8);
    bool fast = flagp && *flagp;
    float dt = g_game_dt.load(std::memory_order_relaxed);
    float ref = fast ? 1.0f / 60 : 1.0f / 30;          // the frame time the constants below are right for
    float k = mode ? dt / ref : 1.0f;
    static uint64_t calls[3]; static int logged;
    if (imm < 3) calls[imm]++;
    if (imm == 1) {          // main+0x102a098: ldr s1, [x8, #0x80]
        float base = fast ? 1.0f / 60 : *(const float *)(c->x[8] + 0x80);
        SET_S(1, base * k);
    } else if (imm == 2) {   // main+0x1260f38: the UI tick (returns s0)
        SET_S(0, (fast ? 1.0f / 120 : 1.0f / 60) * k);
    } else { xrt_trap(c, 0, "hvc"); return; }
    if (logged < 6 && calls[1] + calls[2] == (1ull << (10 + 2 * logged))) {
        logged++;
        LOG(1, "[fps] fixed-step timers: %llu + %llu calls, 60 fps mode %d, frame time %.2f ms, x%.2f",
            (unsigned long long)calls[1], (unsigned long long)calls[2], (int)fast, dt * 1000, k);
    }
}
HLE(queue_present) {     // (queue, window, int textureIndex)
    force_frame_interval();
    fps_cap();
    note_game_frame();
    findctr_frame();   // (XRT_FIND_REPEAT debug tool)
    { static thread_local bool marked; if (!marked) { marked = true; xrt_mark_render_thread(); } }
    order_log("QueuePresent", c);
    g_nvn_frames = ++g_game_frames;
    Job j; j.kind = J_PRESENT;
    {
        RLOCK;
        auto w = g_wins.find(c->x[1]);
        int idx = (int)c->x[2];
        if (w != g_wins.end() && idx >= 0 && idx < (int)w->second.textures.size()) j.a = w->second.textures[idx];
    }
    // at most one frame queued: wait for the previous present to have run (it waited for the GPU slot it reuses)
    static uint64_t prev_present, prev2_present;
    uint64_t seq = dispatch(std::move(j), false);
    if (frame_lag()) { if (prev2_present) wait_done(prev2_present); }   // (two frames queued at most)
    else if (prev_present) wait_done(prev_present);
    prev2_present = prev_present; prev_present = seq;
}

#define N(name, fn) {"nvn:" name, fn}
extern "C" const HleImpl hle_nvn[] = {
    // initialize -> success
    N("nvnDeviceInitialize", ok_true), N("nvnQueueInitialize", ok_true), N("nvnCommandBufferInitialize", ok_true),
    N("nvnSyncInitialize", ok_true), N("nvnProgramInitialize", ok_true), N("nvnSamplerInitialize", sampler_init),
    N("nvnTexturePoolInitialize", ok_true), N("nvnSamplerPoolInitialize", ok_true), N("nvnProgramSetShaders", prog_set_shaders),
    N("nvnMemoryPoolInitialize", mp_init), N("nvnBufferInitialize", buf_init), N("nvnTextureInitialize", tex_init),
    N("nvnWindowInitialize", win_init),
    // builders
    N("nvnDeviceBuilderSetDefaults", b_defaults), N("nvnDeviceBuilderSetFlags", b_flags),
    N("nvnQueueBuilderSetDefaults", b_defaults), N("nvnQueueBuilderSetDevice", nop), N("nvnQueueBuilderSetFlags", b_flags),
    N("nvnQueueBuilderSetCommandMemorySize", nop), N("nvnQueueBuilderSetCommandFlushThreshold", nop), N("nvnQueueBuilderSetControlMemorySize", nop),
    N("nvnMemoryPoolBuilderSetDefaults", b_defaults), N("nvnMemoryPoolBuilderSetDevice", nop), N("nvnMemoryPoolBuilderSetFlags", b_flags),
    N("nvnMemoryPoolBuilderSetStorage", mpb_storage),
    N("nvnBufferBuilderSetDefaults", b_defaults), N("nvnBufferBuilderSetDevice", nop), N("nvnBufferBuilderSetStorage", bb_storage),
    N("nvnTextureBuilderSetDefaults", b_defaults), N("nvnTextureBuilderSetDevice", nop), N("nvnTextureBuilderSetFlags", b_flags),
    N("nvnTextureBuilderSetTarget", tb_target), N("nvnTextureBuilderSetFormat", tb_format), N("nvnTextureBuilderSetSize1D", tb_size1d),
    N("nvnTextureBuilderSetSize2D", tb_size2d), N("nvnTextureBuilderSetSize3D", tb_size3d), N("nvnTextureBuilderSetDepth", tb_depth),
    N("nvnTextureBuilderSetLevels", tb_levels), N("nvnTextureBuilderSetSamples", tb_samples), N("nvnTextureBuilderSetStorage", tb_storage),
    N("nvnTextureBuilderSetPackagedTextureData", tb_packaged), N("nvnTextureBuilderSetSwizzle", tb_swizzle), N("nvnTextureBuilderSetDepthStencilMode", tb_ds_mode),
    N("nvnTextureBuilderGetStorageSize", tb_storage_size), N("nvnTextureBuilderGetStorageAlignment", tb_storage_align),
    N("nvnWindowBuilderSetDefaults", b_defaults), N("nvnWindowBuilderSetDevice", nop), N("nvnWindowBuilderSetNativeWindow", nop),
    N("nvnWindowBuilderSetTextures", wb_textures), N("nvnWindowBuilderSetPresentInterval", nop),
    N("nvnSamplerBuilderSetDefaults", sb_defaults), N("nvnSamplerBuilderSetDevice", nop),
    // objects
    N("nvnMemoryPoolMap", mp_map), N("nvnMemoryPoolGetBufferAddress", mp_addr), N("nvnMemoryPoolGetSize", mp_size),
    N("nvnMemoryPoolGetFlags", mp_flags), N("nvnMemoryPoolFlushMappedRange", nop), N("nvnMemoryPoolInvalidateMappedRange", nop),
    N("nvnBufferMap", buf_map), N("nvnBufferGetAddress", buf_addr), N("nvnBufferGetSize", buf_size),
    N("nvnTextureGetTarget", tex_target), N("nvnTextureGetFormat", tex_format), N("nvnTextureGetFlags", tex_flags),
    N("nvnTextureGetWidth", tex_width), N("nvnTextureGetHeight", tex_height), N("nvnTextureGetDepth", tex_depth),
    N("nvnTextureGetLevels", tex_levels), N("nvnTextureGetStorageSize", tex_storage_size),
    N("nvnTextureWriteTexelsStrided", tex_write_texels_strided), N("nvnTextureWriteTexels", tex_write_texels), N("nvnTextureFlushTexels", nop),
    N("nvnWindowAcquireTexture", win_acquire),
    // device
    N("nvnDeviceGetInteger", dev_get_integer), N("nvnDeviceGetTimestampInNanoseconds", dev_timestamp),
    N("nvnDeviceGetTextureHandle", dev_tex_handle), N("nvnTexturePoolRegisterImage", tp_register_image),
    N("nvnCommandBufferBindImage", cmd_bind_image), N("nvnCommandBufferBindImages", cmd_bind_images), N("nvnDeviceGetImageHandle", dev_image_handle),
    N("nvnDeviceGetSeparateSamplerHandle", dev_sep_handle), N("nvnDeviceGetSeparateTextureHandle", dev_sep_handle),
    N("nvnDeviceSetDepthMode", dev_depth_mode), N("nvnDeviceSetWindowOriginMode", dev_origin_mode),
    N("nvnDeviceRegisterFastClearColor", nop), N("nvnDeviceRegisterFastClearDepth", nop),
    // queue / sync
    N("nvnSyncWait", sync_wait), N("nvnQueueFenceSync", queue_fence_sync), N("nvnCommandBufferFenceSync", cmd_fence_sync), N("nvnCommandBufferWaitSync", cmd_wait_sync), N("nvnQueueFlush", nop), N("nvnQueueFinish", queue_finish),
    N("nvnQueueSubmitCommands", queue_submit), N("nvnQueuePresentTexture", queue_present), N("nvnQueueGetError", ret0),
    N("nvnCommandBufferEndRecording", cmd_end), N("nvnCommandBufferBeginRecording", cmd_begin),
    N("nvnCommandBufferSetRenderTargets", cmd_set_rts), N("nvnCommandBufferClearColor", cmd_clear_color),
    N("nvnCommandBufferClearDepthStencil", cmd_clear_ds),
    N("nvnTexturePoolRegisterTexture", tp_register_tex), N("nvnCommandBufferBindTexture", cmd_bind_texture),
    N("nvnCommandBufferBindTextures", cmd_bind_textures), N("nvnCommandBufferBindProgram", cmd_bind_program),
    N("nvnCommandBufferBindVertexAttribState", cmd_bind_vattr), N("nvnCommandBufferBindVertexStreamState", cmd_bind_vstream),
    N("nvnCommandBufferBindVertexBuffer", cmd_bind_vb), N("nvnCommandBufferBindVertexBuffers", cmd_bind_vbs),
    N("nvnCommandBufferBindUniformBuffer", cmd_bind_ubo), N("nvnCommandBufferBindUniformBuffers", cmd_bind_ubos),
    N("nvnCommandBufferBindBlendState", cmd_bind_blend), N("nvnCommandBufferBindColorState", cmd_bind_color),
    N("nvnCommandBufferBindChannelMaskState", cmd_bind_cmask), N("nvnCommandBufferBindDepthStencilState", cmd_bind_depth),
    N("nvnCommandBufferBindPolygonState", cmd_bind_poly), N("nvnCommandBufferSetViewport", cmd_viewport),
    N("nvnCommandBufferSetViewports", cmd_viewports), N("nvnCommandBufferSetScissor", cmd_scissor),
    N("nvnCommandBufferSetScissors", cmd_scissors), N("nvnCommandBufferSetDepthRange", cmd_depth_range),
    N("nvnCommandBufferSetBlendColor", cmd_blend_color),
    N("nvnColorStateSetAlphaTest", cs_alpha_test), N("nvnPolygonStateSetPolygonOffsetEnables", ps_offset_enables),
    N("nvnCommandBufferSetPolygonOffsetClamp", cmd_poly_offset), N("nvnCommandBufferSetAlphaRef", cmd_alpha_ref),
    N("nvnSamplerBuilderSetBorderColor", sb_border), N("nvnSamplerBuilderSetBorderColori", sb_border), N("nvnSamplerBuilderSetBorderColorui", sb_border),
    N("nvnCommandBufferBindStorageBuffer", cmd_bind_ssbo), N("nvnCommandBufferBindStorageBuffers", cmd_bind_ssbos),
    N("nvnCommandBufferClearColori", cmd_clear_color_int), N("nvnCommandBufferClearColorui", cmd_clear_color_int),
    N("nvnCommandBufferClearBuffer", cmd_clear_buffer),
    N("nvnCommandBufferSetTiledCacheTileSize", nop), N("nvnCommandBufferSetTiledCacheAction", nop),
    N("nvnCommandBufferBarrier", cmd_barrier), N("nvnCommandBufferSetTexturePool", cmd_set_tex_pool), N("nvnCommandBufferReportCounter", cmd_report_counter),
    N("nvnCommandBufferSetRasterizerDiscard", cmd_rast_discard), N("nvnCommandBufferBindTransformFeedbackBuffer", cmd_xfb_bind),
    N("nvnCommandBufferBeginTransformFeedback", cmd_xfb_begin), N("nvnCommandBufferEndTransformFeedback", cmd_xfb_end),
    N("nvnCommandBufferDrawTransformFeedback", cmd_xfb_draw),
    N("nvnCommandBufferResetCounter", cmd_reset_counter), N("nvnCommandBufferSetSamplerPool", cmd_set_smp_pool),
    N("nvnCommandBufferSetShaderScratchMemory", nop), N("nvnCommandBufferSetMemoryCallback", nop),
    N("nvnCommandBufferSetMemoryCallbackData", nop), N("nvnCommandBufferAddCommandMemory", nop), N("nvnCommandBufferAddControlMemory", nop),
    N("nvnMemoryPoolFinalize", nop), N("nvnProgramFinalize", nop), N("nvnSamplerFinalize", nop), N("nvnBufferFinalize", nop),
    N("nvnBufferFlushMappedRange", nop), N("nvnWindowSetCrop", window_set_crop),
    N("nvnVertexAttribStateSetDefaults", va_defaults), N("nvnVertexAttribStateSetFormat", va_format),
    N("nvnVertexAttribStateSetStreamIndex", va_stream), N("nvnVertexStreamStateSetDefaults", vs_defaults),
    N("nvnVertexStreamStateSetStride", vs_stride), N("nvnVertexStreamStateSetDivisor", vs_divisor),
    N("nvnBlendStateSetDefaults", bs_defaults), N("nvnBlendStateSetBlendTarget", bs_target),
    N("nvnBlendStateSetBlendFunc", bs_func), N("nvnBlendStateSetBlendEquation", bs_equation),
    N("nvnColorStateSetDefaults", cs_defaults), N("nvnColorStateSetBlendEnable", cs_blend_enable),
    N("nvnColorStateGetBlendEnable", cs_get_blend_enable), N("nvnColorStateSetLogicOp", cs_logic_op),
    N("nvnChannelMaskStateSetDefaults", cm_defaults), N("nvnChannelMaskStateSetChannelMask", cm_mask),
    N("nvnDepthStencilStateSetDefaults", ds_defaults), N("nvnDepthStencilStateSetDepthTestEnable", ds_test),
    N("nvnDepthStencilStateSetDepthWriteEnable", ds_write), N("nvnDepthStencilStateSetDepthFunc", ds_func),
    N("nvnDepthStencilStateSetStencilTestEnable", ds_stencil),
    N("nvnDepthStencilStateSetStencilFunc", ds_stencil_func), N("nvnDepthStencilStateSetStencilOp", ds_stencil_op),
    N("nvnCommandBufferSetStencilRef", cmd_stencil_ref), N("nvnCommandBufferSetStencilValueMask", cmd_stencil_vmask),
    N("nvnCommandBufferSetStencilMask", cmd_stencil_wmask),
    N("nvnPolygonStateSetDefaults", ps_defaults), N("nvnPolygonStateSetCullFace", ps_cull),
    N("nvnPolygonStateSetFrontFace", ps_front), N("nvnPolygonStateSetPolygonMode", ps_mode),
    N("nvnSamplerBuilderSetWrapMode", sb_wrap), N("nvnSamplerBuilderSetMinMagFilter", sb_minmag),
    N("nvnSamplerBuilderSetMaxAnisotropy", sb_aniso), N("nvnSamplerBuilderSetLodClamp", sb_lod_clamp),
    N("nvnSamplerBuilderSetLodBias", sb_lod_bias), N("nvnSamplerBuilderSetCompare", sb_compare),
    N("nvnSamplerPoolRegisterSampler", sp_register_sampler),
    N("nvnCommandBufferDrawArraysInstanced", cmd_draw_arrays_inst), N("nvnCommandBufferDrawElementsBaseVertex", cmd_draw_elements_bv),
    N("nvnCommandBufferCallCommands", cmd_call), N("nvnCommandBufferCopyCommands", cmd_call),
    N("nvnCommandBufferGetCommandMemoryUsed", cmd_mem_used), N("nvnCommandBufferGetControlMemoryUsed", cmd_ctrl_used),
    N("nvnCommandBufferDrawArrays", cmd_draw_arrays), N("nvnCommandBufferDrawElements", cmd_draw_elements),
    N("nvnCommandBufferDrawElementsInstanced", cmd_draw_elements_inst),
    N("nvnTextureViewSetDefaults", tv_defaults), N("nvnTextureViewSetLevels", tv_levels),
    N("nvnTextureViewSetLayers", tv_layers), N("nvnTextureViewSetFormat", tv_format),
    N("nvnTextureViewSetTarget", tv_target), N("nvnTextureViewSetSwizzle", tv_swizzle), N("nvnTextureFinalize", tex_finalize),
    {nullptr, nullptr}
};
