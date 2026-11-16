static int g_depth_mode = 0;   // NVNdepthMode, set by nvnDeviceSetDepthMode
void gpu_set_depth_mode(int m) { g_depth_mode = m; }
// Draw path of the Vulkan backend (included by gpu_vk.cpp; shares its statics).
//
//  * NVN memory pools are imported as VkBuffers (VK_EXT_external_memory_host): a "GPU address" is a
//    host pointer, so vertex/index/uniform/storage buffers are (pool buffer, offset) with no copies.
//  * Shaders are translated per program by Eden's recompiler (gpu_shader.cpp) with the bound state,
//    then cached; pipelines are cached by the full fixed-function state.
//  * Render passes use dynamic rendering; every image stays in VK_IMAGE_LAYOUT_GENERAL.
#include <map>
#include <memory>
#include <deque>
#include <string>
#include <unordered_map>
#include "gpu_shader.h"
#ifdef _WIN32
#include <direct.h>
#endif

static GpuResolver g_res;
static std::atomic<uint64_t> g_worker_scan_wait;          // worker time waiting for the write-watch scan (ns)
// per-draw timers (draw sub-steps, programs): ~10 clock reads a draw, 0.5 ms a frame on their own - XRT_PROF_DRAW=1
static bool prof_draw() { static int p = -1; if (p < 0) p = getenv("XRT_PROF_DRAW") ? 1 : 0; return p == 1; }
static inline uint64_t PTN() { return prof_draw() ? plat_ticks_ns() : 0; }
static bool desc_cache_off() { static int o = -1; if (o < 0) o = getenv("XRT_NO_DESC_CACHE") ? 1 : 0; return o == 1; }
// descriptor sets already written this frame, by content (with dynamic uniform offsets most draws of a material
// share one): key hash -> (key, set). Cleared with the pools at every frame start.
// (open addressing on the 64-bit content hash, valid for one frame via g_set_epoch: no allocation per set)
struct SetSlot { uint64_t hash, epoch; VkDescriptorSet set; };
static std::vector<SetSlot> g_set_cache(16384); static uint64_t g_set_epoch = 1;
static uint64_t g_set_hits, g_set_misses;
// Driver pipeline cache kept on disk between runs (pipeline_cache.bin next to the game data): pipelines already built
// in an earlier session are not compiled again (M5). Saved every 600 frames when new pipelines were made.
static VkPipelineCache g_pcache = VK_NULL_HANDLE; static uint64_t g_pcache_new;
static VkPipelineCache pipeline_cache() {
    static bool tried; if (tried) return g_pcache; tried = true;
    if (getenv("XRT_NO_PIPELINE_CACHE")) return VK_NULL_HANDLE;
    std::vector<char> data;
    if (FILE *f = fopen("pipeline_cache.bin", "rb")) { fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); if (n > 0) { data.resize((size_t)n); if (fread(data.data(), 1, data.size(), f) != data.size()) data.clear(); } fclose(f); }
    VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = data.size(); ci.pInitialData = data.empty() ? nullptr : data.data();   // (the driver ignores data from another driver / GPU)
    if (vkCreatePipelineCache(g_dev, &ci, nullptr, &g_pcache) != VK_SUCCESS) {
        ci.initialDataSize = 0; ci.pInitialData = nullptr;
        if (vkCreatePipelineCache(g_dev, &ci, nullptr, &g_pcache) != VK_SUCCESS) g_pcache = VK_NULL_HANDLE;
    }
    fprintf(stderr, "[gpu] pipeline cache: %zu bytes loaded\n", data.size());
    return g_pcache;
}
// Saving copies the whole cache (80+ MB) and writes it: done on a thread of its own, at most once a minute, and when
// the window closes (it was every 600 frames on the presenting thread: a 100-140 ms stutter every few seconds).
static std::atomic<bool> g_pc_saving{false};
static void pipeline_cache_save(bool now) {
    static uint64_t last;
    if (!g_pcache || !g_pcache_new) return;
    uint64_t t = plat_ticks_ns();
    if (!now && last && t - last < 60000000000ULL) return;
    if (g_pc_saving.exchange(true)) return;
    last = t; g_pcache_new = 0;
    auto work = [] {
        size_t n = 0;
        if (vkGetPipelineCacheData(g_dev, g_pcache, &n, nullptr) == VK_SUCCESS && n) {
            std::vector<char> data(n);
            if (vkGetPipelineCacheData(g_dev, g_pcache, &n, data.data()) == VK_SUCCESS)
                if (FILE *f = fopen("pipeline_cache.tmp", "wb")) {
                    bool ok = fwrite(data.data(), 1, n, f) == n; fclose(f);
                    if (ok) { remove("pipeline_cache.bin"); rename("pipeline_cache.tmp", "pipeline_cache.bin"); }
                }
        }
        g_pc_saving = false;
    };
    if (now) work(); else std::thread(work).detach();
}
void gpu_save_caches() {   // (window closing)
    for (int i = 0; i < 500 && g_pc_saving.load(); i++) plat_sleep_until_ns(plat_ticks_ns() + 10000000ULL);
    pipeline_cache_save(true);
}
void gpu_set_resolver(const GpuResolver &r) { g_res = r; }

#define log_once(key, ...) do { static std::unordered_map<std::string, int> _seen; if (_seen[key]++ == 0) { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

// ---------------------------------------------------------------- memory pools
// A pool is imported as host memory (VK_EXT_external_memory_host). Pools that draws read as buffers also get a VRAM
// mirror (milestone M5, after xc2-recomp's design): the GPU then reads device memory instead of fetching vertex /
// uniform / storage data over PCIe. At each queue submit the pages the CPU wrote since the last one (Windows write
// watch) are copied by the CPU into staging - a snapshot, so the game may rewrite its uniforms right after
// submitting - and from there into the mirror. GPU writes (skinning, transform feedback, fills) go to the mirror.
// XRT_POOL_MIRROR=0: no mirrors (every buffer read goes to host memory, the old behaviour; also without write watch).
struct PoolBuf {
    uint64_t base = 0, size = 0; VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; bool tried = false;
    VkBuffer mbuf = VK_NULL_HANDLE; VkDeviceMemory mmem = VK_NULL_HANDLE;   // VRAM mirror (once created)
    bool want_mirror = false, mirror_failed = false;
    uint8_t *shadow = nullptr;                                              // frame lag: memory as of the job being translated
    std::vector<void *> ww;                                                 // write-watch page list (capacity = pages)
};
static std::map<uint64_t, PoolBuf> g_poolbufs;
static const VkBufferUsageFlags k_buf_usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
    VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;

void gpu_pool_register(uint64_t base, uint64_t size) { if (base && size) g_poolbufs[base] = PoolBuf{base, size}; }

static bool pool_import(PoolBuf &p) {
    p.tried = true;
    if (!g_host_import || !vkGetMemoryHostPointerPropertiesEXT) { log_once("noimport", "[gpu] VK_EXT_external_memory_host missing: draws disabled"); return false; }
    if ((p.base & 0xfff) || (p.size & 0xfff)) { log_once("align" + std::to_string(p.base), "[gpu] pool %llx+%llx not page aligned", (unsigned long long)p.base, (unsigned long long)p.size); return false; }
    VkMemoryHostPointerPropertiesEXT hp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (vkGetMemoryHostPointerPropertiesEXT(g_dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, (void *)(uintptr_t)p.base, &hp) != VK_SUCCESS) return false;
    VkExternalMemoryBufferCreateInfo eb = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    eb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.pNext = &eb; bci.size = p.size; bci.usage = k_buf_usage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
        (g_xfb ? VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT : 0);
    if (vkCreateBuffer(g_dev, &bci, nullptr, &p.buf) != VK_SUCCESS) return false;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_dev, p.buf, &mr);
    uint32_t bits = mr.memoryTypeBits & hp.memoryTypeBits;
    uint32_t type = mem_type(bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) type = mem_type(bits, 0);
    VkImportMemoryHostPointerInfoEXT imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT; imp.pHostPointer = (void *)(uintptr_t)p.base;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &imp; mai.allocationSize = p.size; mai.memoryTypeIndex = type;
    if (type == UINT32_MAX || vkAllocateMemory(g_dev, &mai, nullptr, &p.mem) != VK_SUCCESS) {
        log_once("imp" + std::to_string(p.base), "[gpu] importing pool %llx (%llu MB) failed", (unsigned long long)p.base, (unsigned long long)(p.size >> 20));
        vkDestroyBuffer(g_dev, p.buf, nullptr); p.buf = VK_NULL_HANDLE; return false;
    }
    vkBindBufferMemory(g_dev, p.buf, p.mem, 0);
    LOG(2, "[gpu] imported pool %llx (%llu KB)", (unsigned long long)p.base, (unsigned long long)(p.size >> 10));
    return true;
}
// guest/host address -> (buffer, offset); `need` bytes must fit in the pool
static bool lookup_buffer(uint64_t addr, uint64_t need, VkBuffer &buf, VkDeviceSize &off) {
    static PoolBuf *last;   // most lookups hit the pool of the previous one (map nodes are stable)
    PoolBuf *pp = last;
    if (!pp || addr < pp->base || addr >= pp->base + pp->size) {
        auto it = g_poolbufs.upper_bound(addr);
        if (it == g_poolbufs.begin()) return false;
        --it;
        pp = &it->second;
        if (addr >= pp->base && addr < pp->base + pp->size) last = pp;
    }
    PoolBuf &p = *pp;
    if (addr < p.base || addr + need > p.base + p.size) return false;
    if (!p.buf && !p.tried) pool_import(p);
    if (!p.buf) return false;
    if (p.mbuf) buf = p.mbuf;
    else { buf = p.buf; if (!p.mirror_failed) p.want_mirror = true; }   // (the mirror is made at the next submit)
    off = addr - p.base;
    return true;
}
static bool mirror_enabled() {
    static int on = -1;
    if (on < 0) {
        if (g_poolbufs.empty()) return false;   // (decided once a pool exists)
        on = getenv("XRT_POOL_MIRROR") && atoi(getenv("XRT_POOL_MIRROR")) == 0 ? 0 : 1;
        if (on) {   // write tracking must work on guest memory, or mirrors would go stale
            void *pg[1]; uint64_t n = 1, gran = 0;
            if (plat_write_watch((void *)(uintptr_t)g_poolbufs.begin()->second.base, 4096, pg, &n, &gran) != 0) on = 0;
            LOG(1, "[gpu] VRAM pool mirrors: %s", on ? "on" : "off (no write tracking)");
        }
    }
    return on == 1;
}
static uint64_t g_mirror_bytes, g_upload_bytes, g_upload_runs;
static bool staging_alloc(VkDeviceSize size, VkBuffer &buf, VkDeviceSize &off, uint8_t *&ptr);
static void mirror_stats(double frames) {
    if (g_mirror_bytes) LOG(1, "[prof] VRAM mirrors: %llu MB, %.2f MB/frame uploaded in %.0f runs", (unsigned long long)(g_mirror_bytes >> 20), g_upload_bytes / 1048576.0 / frames, g_upload_runs / frames);
    g_upload_bytes = g_upload_runs = 0;
    if (g_set_hits + g_set_misses) LOG(1, "[prof] descriptor sets per frame: %.0f written, %.0f reused", g_set_misses / frames, g_set_hits / frames);
    LOG(1, "[prof] worker waits per frame: write-watch scan %.2f ms, object lock (game threads) %.2f ms", g_worker_scan_wait.exchange(0) / 1e6 / frames, gpu_nl_wait_ns(true) / 1e6 / frames);
    g_set_hits = g_set_misses = 0;
}
static bool mirror_create(PoolBuf &p) {
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = p.size; bci.usage = k_buf_usage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
        (g_xfb ? VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT : 0);
    if (vkCreateBuffer(g_dev, &bci, nullptr, &p.mbuf) != VK_SUCCESS) { p.mbuf = VK_NULL_HANDLE; return false; }
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_dev, p.mbuf, &mr);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size; mai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(g_dev, &mai, nullptr, &p.mmem) != VK_SUCCESS) {
        vkDestroyBuffer(g_dev, p.mbuf, nullptr); p.mbuf = VK_NULL_HANDLE; p.mmem = VK_NULL_HANDLE;
        log_once("mirrorfail", "[gpu] VRAM mirror allocation failed (%llu MB): that pool stays in host memory", (unsigned long long)(p.size >> 20));
        return false;
    }
    vkBindBufferMemory(g_dev, p.mbuf, p.mmem, 0);
    p.ww.resize((size_t)(p.size / 4096) + 1);
    g_mirror_bytes += p.size;
    LOG(1, "[gpu] VRAM mirror for pool %llx (%llu MB, %llu MB mirrored in all)", (unsigned long long)p.base, (unsigned long long)(p.size >> 20), (unsigned long long)(g_mirror_bytes >> 20));
    return true;
}
// Queue submit: create the mirrors draws asked for, then bring every mirror up to date with what the CPU wrote.
// Write-watch scans (which pages the CPU wrote) cost ~1 ms a frame over the 1.4 GB of mirrored pools: they run on a
// helper thread, started by the game's queue submit (gpu_mirrors_prescan) while the render worker is still on the
// previous frame; the worker then only copies what the scan found. Pages written after a scan are reported by the next
// one (the copies always read current memory). XRT_MIRROR_SYNC_SCAN=1: scan on the worker as before.
struct ScanPool { uint64_t base, size, gran = 4096; std::vector<void *> ww; std::vector<uint64_t> pending; };
static std::mutex g_scan_m;
static std::condition_variable g_scan_cv, g_scan_done_cv;
static std::vector<ScanPool *> g_scan_pools;          // (g_scan_m)
static uint64_t g_scan_req, g_scan_done;              // (g_scan_m)
static std::atomic<bool> g_mirrors_any{false};
static bool g_snapshots;                              // frame lag: submit-time snapshots (gpu_set_snapshots)
static uint64_t g_pending_scan;                       // scan whose copies are not recorded yet (worker)
static std::unordered_map<uint64_t, ScanPool *> g_scan_of;   // pool base -> its scan entry (worker)
static void scan_thread() {
    uint64_t done = 0;
    for (;;) {
        std::vector<ScanPool *> pools; uint64_t target;
        { std::unique_lock<std::mutex> l(g_scan_m); g_scan_cv.wait(l, [&] { return g_scan_req > done; }); target = g_scan_req; pools = g_scan_pools; }
        for (ScanPool *sp : pools) {
            uint64_t n = sp->ww.size(), gran = 4096;
            if (plat_write_watch((void *)(uintptr_t)sp->base, sp->size, sp->ww.data(), &n, &gran) != 0 || !n) continue;
            std::lock_guard<std::mutex> l(g_scan_m);
            if (gran) sp->gran = gran;
            for (uint64_t i = 0; i < n; i++) sp->pending.push_back((uint64_t)(uintptr_t)sp->ww[i]);
        }
        { std::lock_guard<std::mutex> l(g_scan_m); g_scan_done = done = target; }
        g_scan_done_cv.notify_all();
    }
}
static uint64_t scan_request() {
    static std::once_flag started;
    std::call_once(started, [] { std::thread(scan_thread).detach(); });
    uint64_t g;
    { std::lock_guard<std::mutex> l(g_scan_m); g = ++g_scan_req; }
    g_scan_cv.notify_one();
    return g;
}
static bool scan_async() { static int a = -1; if (a < 0) a = getenv("XRT_MIRROR_SYNC_SCAN") ? 0 : 1; return a == 1; }
uint64_t gpu_mirrors_prescan() { return g_mirrors_any.load(std::memory_order_acquire) && scan_async() && !g_snapshots ? scan_request() : 0; }
// Frame lag (XRT_FRAME_LAG): the game may rewrite a frame's data before the worker translates that frame, so the
// write-watch scan and the copy of the written pages happen at the game's queue submit (gpu_mirrors_snapshot, game
// thread); the worker uploads that snapshot to the VRAM mirrors and applies it to a CPU shadow of each mirrored pool,
// from which the translation reads guest data (uniform words, storage addresses, index data): memory as of the submit.
struct SnapRun { ScanPool *sp; uint64_t addr; uint32_t len; size_t data; };
struct MirrorSnap { std::vector<SnapRun> runs; std::vector<uint8_t> data; };
void gpu_set_snapshots(bool on) { g_snapshots = on; }
void *gpu_mirrors_snapshot() {
    if (!g_snapshots || !g_mirrors_any.load(std::memory_order_acquire)) return nullptr;
    uint64_t gen = scan_request();
    std::vector<std::pair<ScanPool *, std::vector<uint64_t>>> got;
    {
        std::unique_lock<std::mutex> l(g_scan_m);
        g_scan_done_cv.wait(l, [gen] { return g_scan_done >= gen; });
        for (ScanPool *sp : g_scan_pools) if (!sp->pending.empty()) { got.push_back({sp, std::move(sp->pending)}); sp->pending.clear(); }
    }
    MirrorSnap *ms = new MirrorSnap;
    for (auto &g : got) {
        ScanPool *sp = g.first; std::vector<uint64_t> &pages = g.second;
        uint64_t gran = sp->gran ? sp->gran : 4096;
        std::sort(pages.begin(), pages.end()); pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
        for (size_t i = 0; i < pages.size();) {
            uint64_t a = pages[i], e = a + gran; size_t j = i + 1;
            while (j < pages.size() && pages[j] == e) { e += gran; j++; }
            if (e > sp->base + sp->size) e = sp->base + sp->size;
            size_t off = ms->data.size();
            ms->data.resize(off + (size_t)(e - a));
            memcpy(ms->data.data() + off, (const void *)(uintptr_t)a, (size_t)(e - a));
            ms->runs.push_back({sp, a, (uint32_t)(e - a), off});
            i = j;
        }
    }
    return ms;
}
// guest memory as the translation should see it: the pool's shadow under frame lag, else the memory itself
static const uint8_t *gview(uint64_t addr) {
    if (!g_snapshots) return (const uint8_t *)(uintptr_t)addr;
    static PoolBuf *last;
    PoolBuf *p = last;
    if (!p || addr < p->base || addr >= p->base + p->size) {
        auto it = g_poolbufs.upper_bound(addr);
        if (it == g_poolbufs.begin()) return (const uint8_t *)(uintptr_t)addr;
        --it; p = &it->second;
        if (addr < p->base || addr >= p->base + p->size) return (const uint8_t *)(uintptr_t)addr;
        last = p;
    }
    return p->shadow ? p->shadow + (addr - p->base) : (const uint8_t *)(uintptr_t)addr;
}
static uint32_t GV32(uint64_t a) { uint32_t v; memcpy(&v, gview(a), 4); return v; }
static uint64_t GV64(uint64_t a) { uint64_t v; memcpy(&v, gview(a), 8); return v; }
void gpu_mirrors_sync(uint64_t gen, void *snap_) {
    MirrorSnap *snap = (MirrorSnap *)snap_;
    if (!gpu_available() || !mirror_enabled()) { delete snap; return; }
    bool begun = false;
    auto begin = [&] { if (!begun) { gpu_end_pass(); begin_frame(); full_barrier(); begun = true; } };
    for (auto &kv : g_poolbufs) {   // new mirrors: filled from host memory, then watched by the scan thread
        PoolBuf &p = kv.second;
        if (!p.buf || p.mbuf || !p.want_mirror || p.mirror_failed) continue;
        if (!mirror_create(p)) { p.mirror_failed = true; continue; }
        uint64_t n = p.ww.size(), gran = 0;
        plat_write_watch((void *)(uintptr_t)p.base, p.size, p.ww.data(), &n, &gran);   // reset: the full copy covers it
        begin();
        VkBufferCopy bc = {0, 0, p.size};
        vkCmdCopyBuffer(g_cmd, p.buf, p.mbuf, 1, &bc);   // initial contents (host memory, read by the GPU)
        g_upload_bytes += p.size;
        if (g_snapshots) { p.shadow = (uint8_t *)plat_alloc(p.size); memcpy(p.shadow, (const void *)(uintptr_t)p.base, (size_t)p.size); }
        ScanPool *sp = new ScanPool; sp->base = p.base; sp->size = p.size; sp->ww.resize(p.ww.size());
        { std::lock_guard<std::mutex> l(g_scan_m); g_scan_pools.push_back(sp); }
        g_scan_of[p.base] = sp;
        g_mirrors_any.store(true, std::memory_order_release);
    }
    if (snap) {   // frame lag: the pages copied at the game's submit
        for (const SnapRun &r : snap->runs) {
            auto pi = g_poolbufs.find(r.sp->base);
            if (pi == g_poolbufs.end() || !pi->second.mbuf) continue;
            PoolBuf &p = pi->second;
            const uint8_t *src = snap->data.data() + r.data;
            if (p.shadow) memcpy(p.shadow + (r.addr - p.base), src, r.len);
            begin();
            VkBuffer sb; VkDeviceSize so; uint8_t *spp;
            if (staging_alloc(r.len, sb, so, spp)) {
                memcpy(spp, src, r.len);
                VkBufferCopy bc = {so, r.addr - p.base, r.len};
                vkCmdCopyBuffer(g_cmd, sb, p.mbuf, 1, &bc);
            }
            g_upload_bytes += r.len; g_upload_runs++;
        }
        delete snap;
        if (begun) full_barrier();
        return;
    }
    if (g_scan_of.empty() || g_snapshots) { if (begun) full_barrier(); return; }   // (frame lag: only snapshots carry writes)
    if (!gen) gen = scan_request();   // (no scan started at the game's submit: run one now)
    if (gen > g_pending_scan) g_pending_scan = gen;   // copies recorded at the frame's first submission (mirrors_flush)
    if (begun) full_barrier();
}
// The written pages are copied in a command buffer of their own, submitted just before the frame's first submission
// (a mid-frame one or present): the worker translates the first draws while the scan is still running instead of
// waiting for it (~1.7 ms a frame). Draws read the mirrors only on the GPU, after these copies (queue order + barrier).
static void mirrors_flush() {
    if (!g_pending_scan) return;
    uint64_t gen = g_pending_scan; g_pending_scan = 0;
    std::unique_lock<std::mutex> l(g_scan_m);
    { uint64_t tw = plat_ticks_ns(); g_scan_done_cv.wait(l, [gen] { return g_scan_done >= gen; }); g_worker_scan_wait += plat_ticks_ns() - tw; }
    static std::vector<uint64_t> pages;
    VkCommandBuffer pc = VK_NULL_HANDLE;
    for (auto &kv : g_poolbufs) {
        PoolBuf &p = kv.second;
        if (!p.mbuf) continue;
        auto si = g_scan_of.find(p.base);
        if (si == g_scan_of.end()) continue;
        ScanPool *sp = si->second;
        if (sp->pending.empty()) continue;
        pages.swap(sp->pending); sp->pending.clear();
        uint64_t gran = sp->gran ? sp->gran : 4096;
        l.unlock();
        std::sort(pages.begin(), pages.end()); pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
        if (!pc) {
            FrameSlot &fs = g_slots[g_slot];
            if (fs.extra_used == fs.extra.size()) {
                VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                ai.commandPool = g_pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
                VkCommandBuffer cb = VK_NULL_HANDLE;
                if (vkAllocateCommandBuffers(g_dev, &ai, &cb) != VK_SUCCESS) { pages.clear(); l.lock(); continue; }
                fs.extra.push_back(cb);
            }
            pc = fs.extra[fs.extra_used++];
            VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(pc, &bi);
            VkMemoryBarrier mb0 = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};   // (earlier frames' reads of these ranges first)
            mb0.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT; mb0.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(pc, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb0, 0, nullptr, 0, nullptr);
        }
        for (size_t i = 0; i < pages.size();) {   // runs of consecutive dirty pages: CPU snapshot -> staging -> mirror
            uint64_t a = pages[i], e = a + gran; size_t j = i + 1;
            while (j < pages.size() && pages[j] == e) { e += gran; j++; }
            if (e > p.base + p.size) e = p.base + p.size;
            VkBuffer sb; VkDeviceSize so; uint8_t *spp;
            if (staging_alloc(e - a, sb, so, spp)) {
                memcpy(spp, (const void *)(uintptr_t)a, (size_t)(e - a));
                VkBufferCopy bc = {so, a - p.base, e - a};
                vkCmdCopyBuffer(pc, sb, p.mbuf, 1, &bc);
            } else {   // (no staging: copy from host memory on the GPU, without the snapshot)
                VkBufferCopy bc = {a - p.base, a - p.base, e - a};
                vkCmdCopyBuffer(pc, p.buf, p.mbuf, 1, &bc);
            }
            g_upload_bytes += e - a; g_upload_runs++;
            i = j;
        }
        pages.clear();
        l.lock();
    }
    l.unlock();
    if (!pc) return;
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(pc, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    vkEndCommandBuffer(pc);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &pc;
    vkQueueSubmit(g_queue, 1, &si, VK_NULL_HANDLE);
}
static VkResult submit_frame(const VkSubmitInfo *si, VkFence f) { mirrors_flush(); return vkQueueSubmit(g_queue, 1, si, f); }

// ---------------------------------------------------------------- per-frame resources
static bool lookup_buffer(uint64_t addr, uint64_t need, VkBuffer &buf, VkDeviceSize &off);
void gpu_fill_buffer(uint64_t addr, uint64_t size, uint32_t value) {
    if (!gpu_available()) return;
    VkBuffer buf; VkDeviceSize off;
    if (!size || (addr & 3) || !lookup_buffer(addr, size, buf, off)) return;
    gpu_end_pass(); begin_frame(); full_barrier();
    vkCmdFillBuffer(g_cmd, buf, off, size & ~3ULL, value);
}
struct Staging { VkBuffer buf; VkDeviceMemory mem; uint8_t *map; VkDeviceSize size, used; };
static std::vector<Staging> g_staging, g_dynidx;   // upload staging / generated index data, recycled each frame
static std::vector<VkDescriptorPool> g_dpools; static size_t g_dpool_cur;
// per frame slot (gpu_vk.cpp FrameSlot): the rings / pools in use stay with their slot until its fence
static struct { std::vector<Staging> staging, dynidx; std::vector<VkDescriptorPool> dpools; } g_slot_res[NSLOT];
static void slot_stash_resources(int i) { g_slot_res[i].staging.swap(g_staging); g_slot_res[i].dynidx.swap(g_dynidx); g_slot_res[i].dpools.swap(g_dpools); }
static void slot_restore_resources(int i) { g_slot_res[i].staging.swap(g_staging); g_slot_res[i].dynidx.swap(g_dynidx); g_slot_res[i].dpools.swap(g_dpools); }
static void frame_done() {
    for (Staging &s : g_staging) s.used = 0;
    for (Staging &s : g_dynidx) s.used = 0;
    for (VkDescriptorPool p : g_dpools) vkResetDescriptorPool(g_dev, p, 0);
    g_dpool_cur = 0;
    g_set_epoch++;
}
static bool ring_alloc(std::vector<Staging> &ring, VkBufferUsageFlags usage, VkDeviceSize size, VkBuffer &buf, VkDeviceSize &off, uint8_t *&ptr) {
    size = (size + 15) & ~15ULL;
    for (Staging &s : ring)
        if (s.size - s.used >= size) { buf = s.buf; off = s.used; ptr = s.map + s.used; s.used += size; return true; }
    Staging s{}; s.size = size > (32ULL << 20) ? size : (32ULL << 20);
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = s.size; bci.usage = usage;
    if (vkCreateBuffer(g_dev, &bci, nullptr, &s.buf) != VK_SUCCESS) return false;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_dev, s.buf, &mr);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size; mai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(g_dev, &mai, nullptr, &s.mem) != VK_SUCCESS) return false;
    vkBindBufferMemory(g_dev, s.buf, s.mem, 0);
    void *m; vkMapMemory(g_dev, s.mem, 0, VK_WHOLE_SIZE, 0, &m); s.map = (uint8_t *)m;
    s.used = size; ring.push_back(s);
    buf = s.buf; off = 0; ptr = s.map;
    return true;
}
static bool staging_alloc(VkDeviceSize size, VkBuffer &buf, VkDeviceSize &off, uint8_t *&ptr) {
    return ring_alloc(g_staging, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, size, buf, off, ptr);
}
static VkDescriptorSet alloc_set(VkDescriptorSetLayout l) {
    for (;;) {
        if (g_dpool_cur == g_dpools.size()) {
            VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32768}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32768},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8192}, {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 2048},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 2048}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2048},
                                            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 32768}};
            VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pci.maxSets = 8192; pci.poolSizeCount = 7; pci.pPoolSizes = sizes;
            VkDescriptorPool p; if (vkCreateDescriptorPool(g_dev, &pci, nullptr, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
            g_dpools.push_back(p);
        }
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = g_dpools[g_dpool_cur]; ai.descriptorSetCount = 1; ai.pSetLayouts = &l;
        VkDescriptorSet set;
        if (vkAllocateDescriptorSets(g_dev, &ai, &set) == VK_SUCCESS) return set;
        g_dpool_cur++;
    }
}

// ---------------------------------------------------------------- texture uploads + views
bool gpu_tex_upload(GpuTex *t, int level, int layer, const void *data, size_t size) {
    if (!t || level >= t->levels || layer >= t->layers) return false;
    gpu_end_pass();
    begin_frame();
    VkBuffer buf; VkDeviceSize off; uint8_t *ptr;
    if (!staging_alloc(size, buf, off, ptr)) return false;
    memcpy(ptr, data, size);
    full_barrier();
    VkBufferImageCopy bc = {};
    bc.bufferOffset = off;
    bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)level, (uint32_t)layer, 1};
    bc.imageExtent = {(uint32_t)std::max(1, t->w >> level), (uint32_t)std::max(1, t->h >> level), 1};
    vkCmdCopyBufferToImage(g_cmd, buf, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
    return true;
}
bool gpu_tex_upload_3d(GpuTex *t, int level, int depth, const void *data, size_t size) {
    if (!t || !t->depth3 || level >= t->levels) return false;
    gpu_end_pass();
    begin_frame();
    VkBuffer buf; VkDeviceSize off; uint8_t *ptr;
    if (!staging_alloc(size, buf, off, ptr)) return false;
    memcpy(ptr, data, size);
    full_barrier();
    VkBufferImageCopy bc = {};
    bc.bufferOffset = off;
    bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)level, 0, 1};
    bc.imageExtent = {(uint32_t)std::max(1, t->w >> level), (uint32_t)std::max(1, t->h >> level), (uint32_t)depth};
    vkCmdCopyBufferToImage(g_cmd, buf, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
    return true;
}
static VkImageView make_view(VkImage img, VkImageViewType type, VkFormat fmt, VkImageAspectFlags aspect, uint32_t base_layer, uint32_t layers, uint32_t levels, uint32_t base_level = 0, const VkComponentMapping *cm = nullptr) {
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img; vi.viewType = type; vi.format = fmt;
    if (cm) vi.components = *cm;
    vi.subresourceRange = {aspect, base_level, levels, base_layer, layers};
    VkImageView v = VK_NULL_HANDLE;
    vkCreateImageView(g_dev, &vi, nullptr, &v);
    return v;
}
// sampled view matching the shader's texture type (null if the image can't provide it)
static VkImageView tex_view(GpuTex *t, int tt) {
    if (tt < 0 || tt > 8) return VK_NULL_HANDLE;
    if (t->views[tt]) return t->views[tt];
    VkImageAspectFlags aspect = t->depth_kind ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    if (t->depth_kind == 3) aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
    VkImageViewType vt; uint32_t layers = 1;
    switch (tt) {
    case TT_2D: case TT_2D_RECT: vt = VK_IMAGE_VIEW_TYPE_2D; break;
    case TT_2D_ARRAY: vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY; layers = (uint32_t)t->layers; break;
    case TT_CUBE: if (t->layers < 6 || t->w != t->h) return VK_NULL_HANDLE; vt = VK_IMAGE_VIEW_TYPE_CUBE; layers = 6; break;
    case TT_CUBE_ARRAY: if (t->layers < 6 || t->w != t->h) return VK_NULL_HANDLE; vt = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY; layers = (uint32_t)(t->layers / 6 * 6); break;
    case TT_3D: if (!t->depth3) return VK_NULL_HANDLE; vt = VK_IMAGE_VIEW_TYPE_3D; break;
    default: return VK_NULL_HANDLE;      // 1D / 3D / buffer: not supported yet
    }
    return t->views[tt] = make_view(t->image, vt, t->format, aspect, 0, layers, (uint32_t)t->levels);
}
// sampled view with an NVN swizzle (NVNtextureSwizzle 0 ZERO, 1 ONE, 2 R, 3 G, 4 B, 5 A)
static VkImageView tex_view_swz(GpuTex *t, int tt, uint32_t swz) {
    if (swz == 0x05040302 || t->depth_kind) return tex_view(t, tt);
    uint32_t key = (uint32_t)tt | swz << 8;
    for (auto &sv : t->swz_views) if (sv.first == key) return sv.second;
    VkImageView base = tex_view(t, tt);      // validates the type and gives layer count
    if (!base) return VK_NULL_HANDLE;
    static const VkComponentSwizzle m[6] = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_R,
                                            VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
    VkComponentMapping cm;
    VkComponentSwizzle *c[4] = {&cm.r, &cm.g, &cm.b, &cm.a};
    for (int i = 0; i < 4; i++) { uint32_t v = (swz >> (8 * i)) & 0xff; *c[i] = v < 6 ? m[v] : VK_COMPONENT_SWIZZLE_IDENTITY; }
    VkImageViewType vt; uint32_t layers = 1;
    switch (tt) {
    case TT_2D_ARRAY: vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY; layers = (uint32_t)t->layers; break;
    case TT_CUBE: vt = VK_IMAGE_VIEW_TYPE_CUBE; layers = 6; break;
    case TT_CUBE_ARRAY: vt = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY; layers = (uint32_t)(t->layers / 6 * 6); break;
    case TT_3D: vt = VK_IMAGE_VIEW_TYPE_3D; break;
    default: vt = VK_IMAGE_VIEW_TYPE_2D; break;
    }
    VkImageView v = make_view(t->image, vt, t->format, VK_IMAGE_ASPECT_COLOR_BIT, 0, layers, (uint32_t)t->levels, 0, &cm);
    t->swz_views.push_back({key, v});
    return v;
}
// storage image view of one mip level (2D / 2D array / 3D as the shader declares it)
static VkImageView storage_view(GpuTex *t, int level, int tt) {
    if (!t->storage || t->depth_kind || level < 0 || level >= t->levels || level >= 16) return VK_NULL_HANDLE;
    if (t->storage_views[level]) return t->storage_views[level];
    VkImageViewType vt = VK_IMAGE_VIEW_TYPE_2D; uint32_t layers = 1;
    if (t->depth3) vt = VK_IMAGE_VIEW_TYPE_3D;
    else if (tt == TT_2D_ARRAY || t->layers > 1) { vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY; layers = (uint32_t)t->layers; }
    return t->storage_views[level] = make_view(t->image, vt, t->format, VK_IMAGE_ASPECT_COLOR_BIT, 0, layers, 1, (uint32_t)level);
}
// depth-stencil texture sampled in stencil mode: a STENCIL-aspect view (integer, one per GpuTex)
// A draw sampling a texture it also renders into (e.g. XC3's cloud puffs read the bound depth buffer) is
// undefined in Vulkan and shows as flickering tiles: like Eden, sample an up-to-date copy instead.
static GpuTex *feedback_copy(GpuTex *t) {
    if (t->depth3) return t;
    if (!t->fb_copy) {
        t->fb_copy = tex_clone(t);
        if (!t->fb_copy) return t;
        static int n; if (n++ < 8) LOG(1, "[gpu] feedback loop: sampling a copy of a %dx%d render target (format %d)", t->w, t->h, (int)t->format);
    }
    if (t->copy_serial != t->write_serial) {
        gpu_end_pass();
        begin_frame(); full_barrier();
        VkImageCopy rg[16]; uint32_t n = 0;
        for (int l = 0; l < t->levels && l < 16; l++) {
            VkImageCopy &c = rg[n++]; c = {};
            c.srcSubresource = {aspect_of(t), (uint32_t)l, 0, (uint32_t)t->layers}; c.dstSubresource = c.srcSubresource;
            c.extent = {(uint32_t)std::max(1, t->w >> l), (uint32_t)std::max(1, t->h >> l), 1};
        }
        g_cnt_fbcopy++; pp_op_begin("feedback copy", t->w, t->h);
        vkCmdCopyImage(g_cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, t->fb_copy->image, VK_IMAGE_LAYOUT_GENERAL, n, rg);
        pp_op_end();
        full_barrier();
        t->copy_serial = t->write_serial;
    }
    return t->fb_copy;
}
static VkImageView stencil_view(GpuTex *t) {
    if (t->depth_kind < 2) return VK_NULL_HANDLE;
    if (!t->views[10]) t->views[10] = make_view(t->image, VK_IMAGE_VIEW_TYPE_2D, t->format, VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 1);
    return t->views[10];
}
static VkImageView rt_view(GpuTex *t, int layer, int level = 0) {
    int n = t->depth3 ? t->depth3 : t->layers;      // 3D: one 2D view per slice (2D_ARRAY_COMPATIBLE)
    if (layer < 0 || layer >= n) layer = 0;
    if (level < 0 || level >= t->levels || level >= 16 || t->depth3) level = 0;
    size_t idx = (size_t)layer * 16 + (size_t)level;
    if (t->rt_views.size() <= idx) t->rt_views.resize(idx + 1, VK_NULL_HANDLE);
    if (!t->rt_views[idx]) t->rt_views[idx] = make_view(t->image, VK_IMAGE_VIEW_TYPE_2D, t->format, aspect_of(t), (uint32_t)layer, 1, 1, (uint32_t)level);
    return t->rt_views[idx];
}
static uint32_t mip_dim(int d, int level) { return (uint32_t)std::max(1, d >> std::max(0, level)); }

// dummy resources for descriptors we can't resolve
static GpuTex *g_dummy_tex; static VkImage g_dummy3d_img; static VkImageView g_dummy3d;
static VkBuffer g_dummy_buf; static VkDeviceMemory g_dummy_mem; static VkBufferView g_dummy_texel;
static GpuTex *g_dummy_storage;
static void make_dummies() {
    if (g_dummy_tex) return;
    GpuTexDesc d{37, 1, 1, 6, 1, 1, 9};
    g_dummy_tex = gpu_tex_create(d);
    uint32_t white[6] = {0, 0, 0, 0, 0, 0};
    for (int l = 0; l < 6; l++) gpu_tex_upload(g_dummy_tex, 0, l, &white[l], 4);
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = 65536; bci.usage = k_buf_usage | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
    vkCreateBuffer(g_dev, &bci, nullptr, &g_dummy_buf);
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_dev, g_dummy_buf, &mr);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size; mai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(g_dev, &mai, nullptr, &g_dummy_mem);
    vkBindBufferMemory(g_dev, g_dummy_buf, g_dummy_mem, 0);
    void *m; vkMapMemory(g_dev, g_dummy_mem, 0, VK_WHOLE_SIZE, 0, &m); memset(m, 0, 65536);
    VkBufferViewCreateInfo bv = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
    bv.buffer = g_dummy_buf; bv.format = VK_FORMAT_R32_UINT; bv.range = VK_WHOLE_SIZE;
    vkCreateBufferView(g_dev, &bv, nullptr, &g_dummy_texel);
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_3D; ci.format = VK_FORMAT_R8G8B8A8_UNORM; ci.extent = {1, 1, 1}; ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    vkCreateImage(g_dev, &ci, nullptr, &g_dummy3d_img);
    vkGetImageMemoryRequirements(g_dev, g_dummy3d_img, &mr);
    VkDeviceMemory m3; mai.allocationSize = mr.size; mai.memoryTypeIndex = mem_type(mr.memoryTypeBits, 0);
    vkAllocateMemory(g_dev, &mai, nullptr, &m3); vkBindImageMemory(g_dev, g_dummy3d_img, m3, 0);
    begin_frame(); image_barrier(g_dummy3d_img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    g_dummy3d = make_view(g_dummy3d_img, VK_IMAGE_VIEW_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1);
    GpuTexDesc sd{11, 1, 1, 1, 1, 1, 1};     // R32UI storage image
    g_dummy_storage = gpu_tex_create(sd);
}
static VkImageView dummy_view(int tt) {
    if (tt == TT_3D) return g_dummy3d;
    if (tt == TT_2D_ARRAY) return tex_view(g_dummy_tex, TT_2D_ARRAY);
    if (tt == TT_CUBE) return tex_view(g_dummy_tex, TT_CUBE);
    if (tt == TT_CUBE_ARRAY) return tex_view(g_dummy_tex, TT_CUBE_ARRAY);
    return tex_view(g_dummy_tex, TT_2D);
}

// ---------------------------------------------------------------- samplers
// Caches keyed by raw bytes (pipeline / program / sampler keys), looked up on every draw: a fast word-wise hash
// and a full-key check, with no allocation on a hit. Keys that collide on the hash go to a std::string map.
static inline uint64_t key_hash(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p; uint64_t h = 0x9E3779B97F4A7C15ull ^ n; size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, b + i, 8); h = (h ^ w) * 0xff51afd7ed558ccdull; h ^= h >> 32; }
    for (; i < n; i++) { h = (h ^ b[i]) * 0x100000001b3ull; }
    return h ^ (h >> 29);
}
template <class V> struct KeyCache {
    std::unordered_map<uint64_t, std::pair<std::string, V>> fast;
    std::unordered_map<std::string, V> slow;
    V *find(const void *k, size_t n) {
        auto it = fast.find(key_hash(k, n));
        if (it != fast.end() && it->second.first.size() == n && !memcmp(it->second.first.data(), k, n)) return &it->second.second;
        if (slow.empty()) return nullptr;
        auto s = slow.find(std::string((const char *)k, n));
        return s != slow.end() ? &s->second : nullptr;
    }
    V &insert(const void *k, size_t n, V v) {
        uint64_t h = key_hash(k, n);
        auto it = fast.find(h);
        if (it == fast.end()) return fast.emplace(h, std::make_pair(std::string((const char *)k, n), std::move(v))).first->second.second;
        return slow[std::string((const char *)k, n)] = std::move(v);
    }
    size_t size() const { return fast.size() + slow.size(); }
};
static KeyCache<VkSampler> g_samplers_vk;
static VkSamplerAddressMode wrap_mode(int w) {
    // NVNwrapMode (the numbering Monado's XCDE port verified, T149). Vulkan has no CLAMP (GL: edge/border blend) nor
    // mirror-once-then-border modes: the nearest edge variants stand in.
    {   static uint32_t seen; if (w >= 0 && w < 32 && !(seen & (1u << w))) { seen |= 1u << w; LOG(1, "[gpu] sampler wrap mode %d in use", w); } }
    switch (w) {
    case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2: case 3: case 4: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 6: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;    // 0 CLAMP, 7 CLAMP_TO_EDGE
    }
}
// NVN border colour -> the nearest built-in Vulkan one (no VK_EXT_custom_border_color yet)
static VkBorderColor border_color(const float c[4]) {
    if (c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 0) return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 1) return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    if (c[0] == 1 && c[1] == 1 && c[2] == 1 && c[3] == 1) return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    static int n; if (n++ < 8) LOG(1, "[gpu] custom sampler border colour %g %g %g %g (approximated)", c[0], c[1], c[2], c[3]);
    if (!(c[3] >= 0.5f)) return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    return c[0] + c[1] + c[2] >= 1.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
}
static VkSampler get_sampler(const GpuSampler &s) {
    if (VkSampler *hit = g_samplers_vk.find(&s, sizeof s)) return *hit;
    VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = s.mag == 0 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    // NVNminFilter: NEAREST, LINEAR, then *_MIPMAP_{NEAREST,LINEAR} variants
    ci.minFilter = (s.min == 0 || s.min == 2 || s.min == 4) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.mipmapMode = (s.min == 4 || s.min == 5) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = wrap_mode(s.wrap[0]); ci.addressModeV = wrap_mode(s.wrap[1]); ci.addressModeW = wrap_mode(s.wrap[2]);
    ci.mipLodBias = s.lod_bias;
    ci.anisotropyEnable = s.max_aniso > 1.0f ? VK_TRUE : VK_FALSE;
    ci.maxAnisotropy = std::min(s.max_aniso, g_limits.maxSamplerAnisotropy > 0 ? g_limits.maxSamplerAnisotropy : 1.0f);
    ci.compareEnable = s.compare ? VK_TRUE : VK_FALSE;
    ci.compareOp = (VkCompareOp)std::max(0, std::min(7, s.compare_func - 1));
    ci.minLod = s.lod_min;
    ci.maxLod = (s.min <= 1) ? std::min(s.lod_min + 0.25f, s.lod_max) : s.lod_max;   // no mip filter: base level only
    ci.borderColor = border_color(s.border);
    VkSampler smp = VK_NULL_HANDLE;
    vkCreateSampler(g_dev, &ci, nullptr, &smp);
    return g_samplers_vk.insert(&s, sizeof s, smp);
}

// ---------------------------------------------------------------- shader programs
struct ProgObj {
    bool ok = false;
    ShaderProgramDesc desc;
    VkShaderModule mod[GS_COUNT] = {};
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderStageFlags pc_stages = 0;
    bool dyn_ubo = false;   // uniform buffers bound with dynamic offsets (descriptor sets reusable across draws)
};
static KeyCache<std::unique_ptr<ProgObj>> g_progs;
static const VkShaderStageFlagBits k_vk_stage[GS_COUNT] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
    VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};

static const GpuDrawState *g_q_state;
// Driver constant bank 0: storage buffer table at 0x110 + 0x400 * NVN stage index + 16 * slot
// (u64 address, u32 size) - measured on the captured shaders (vertex 0x110, fragment 0x510).
static const int k_nvn_stage_index[GS_COUNT] = {0, 3, 4, 2, 1};   // GpuStage -> NVNshaderStage
static uint32_t ssbo_base(int stage) { return 0x110 + 0x400 * (uint32_t)k_nvn_stage_index[stage]; }
static uint32_t c0_word(const GpuDrawState &s, int stage, uint32_t off) {
    uint32_t base = ssbo_base(stage);
    if (off < base || off >= base + 16 * 16) return 0;
    uint32_t slot = (off - base) / 16, w = (off - base) % 16;
    if (w == 0) return (uint32_t)s.ssbo_addr[stage][slot];
    if (w == 4) return (uint32_t)(s.ssbo_addr[stage][slot] >> 32);
    if (w == 8) return (uint32_t)s.ssbo_size[stage][slot];
    return 0;
}
static uint32_t q_read_cbuf(void *, int stage, uint32_t bank, uint32_t off) {
    const GpuDrawState &s = *g_q_state;
    if (bank == 0) return c0_word(s, stage, off);
    if (bank == SHADER_BOUND_TEX_BANK && off >= SHADER_BOUND_IMG_OFF && off < SHADER_BOUND_IMG_OFF + 64) {
        uint64_t h = s.bound_img[stage][(off - SHADER_BOUND_IMG_OFF) / 8];
        return (off & 4) ? (uint32_t)(h >> 32) : (uint32_t)h;
    }
    if (bank == SHADER_BOUND_TEX_BANK) {
        int slot = bound_tex_slot(off);
        if (slot < 0 || slot >= 32) return 0;
        uint64_t h = s.bound_tex[stage][slot];
        return ((off - 0x20) & 4) ? (uint32_t)(h >> 32) : (uint32_t)h;
    }
    if (bank >= 18 || !s.ubo_addr[stage][bank] || off + 4 > s.ubo_size[stage][bank]) return 0;
    return GV32(s.ubo_addr[stage][bank] + off);
}
static int q_texture_type(void *, uint32_t handle) {
    int target = g_res.texture_target ? g_res.texture_target(g_res.user, handle & 0xfffff) : -1;
    switch (target) {   // NVNtextureTarget -> ShaderTexType
    case 0: return TT_1D; case 2: return TT_3D; case 3: return TT_1D_ARRAY; case 4: case 6: return TT_2D_ARRAY;
    case 8: return TT_CUBE; case 9: return TT_CUBE_ARRAY; case 10: return TT_BUFFER;
    default: return TT_2D;
    }
}
static bool q_texture_integer(void *, uint32_t handle) {
    return g_res.texture_flags && (g_res.texture_flags(g_res.user, handle & 0xfffff) & 1);
}
static uint8_t attrib_type_uncached(int fmt);
static uint8_t attrib_type(int fmt) {   // per format, once (it was a string search per attribute per draw)
    static uint8_t cache[1024]; static bool known[1024];
    if (fmt < 0 || fmt >= 1024) return attrib_type_uncached(fmt);
    if (!known[fmt]) { cache[fmt] = attrib_type_uncached(fmt); known[fmt] = true; }
    return cache[fmt];
}
static uint8_t attrib_type_uncached(int fmt) {
    // RGB formats (vertex only): 25 RGB8, 26 RGB8SN, 27 RGB8UI, 28 RGB8I, 29 RGB16F, 30 RGB16, 31 RGB16SN,
    // 32 RGB16UI, 33 RGB16I, 34 RGB32F, 35 RGB32UI, 36 RGB32I
    if (fmt == 27 || fmt == 32 || fmt == 35) return AT_UINT;
    if (fmt == 28 || fmt == 33 || fmt == 36) return AT_SINT;
    if (fmt >= 25 && fmt <= 36) return AT_FLOAT;
    const NvnFormat *f = nvn_format(fmt);
    if (!f) return AT_FLOAT;
    std::string n = f->name;
    if (n.find("UI") != std::string::npos) return AT_UINT;
    if (n.size() > 1 && n.back() == 'I') return AT_SINT;
    return AT_FLOAT;
}
// NVN alpha test (ColorState + nvnCommandBufferSetAlphaRef): a fragment shader variant through Eden's
// RuntimeInfo.alpha_test_func. 0 = none, else NVNalphaFunc 1 NEVER .. 7 GEQUAL. XRT_NO_ALPHA_TEST=1: off.
static int alpha_test_func(const GpuDrawState &s) {
    static int off = -1; if (off < 0) off = getenv("XRT_NO_ALPHA_TEST") ? 1 : 0;
    if (off || s.alpha_func < 1 || s.alpha_func > 7 || !s.shader[GS_FRAGMENT]) return 0;
    { static uint32_t seen; if (!(seen & (1u << s.alpha_func))) { seen |= 1u << s.alpha_func; LOG(1, "[gpu] alpha test func %d in use (ref %g)", s.alpha_func, s.alpha_ref); } }
    return s.alpha_func;
}
// ---------------------------------------------------------------- translated-shader cache on disk (M5: stutter)
// Translating a program (Eden's recompiler) costs milliseconds; done the first time each program is drawn, every
// session, it was the "stutters here and there". shader_cache.bin (next to pipeline_cache.bin) keeps each translation
// with the answers the translator got from the GPU state (constant buffer words, texture types): a later session
// replays those questions against the current state and reuses the SPIR-V when every answer is the same.
// Append-only, so a crash loses nothing. XRT_NO_SHADER_CACHE=1: off.
struct SQuery { uint8_t kind, stage; uint16_t pad; uint32_t a, b, r; };   // kind 0 cbuf(bank a, offset b), 1 tex type(a), 2 tex integer(a)
struct SCacheEntry { std::vector<SQuery> q; ShaderProgramDesc desc; };
static std::unordered_map<uint64_t, std::vector<SCacheEntry>> g_scache;
static FILE *g_scache_f; static int g_scache_state;   // 0 not loaded, 1 on, -1 off
static std::vector<SQuery> *g_q_rec;
extern std::atomic<uint64_t> g_io_ns, g_io_bytes, g_io_reads, g_io_missing, g_tex_new, g_tex_new_ns;
static uint64_t g_hitch_prog_new, g_hitch_prog_disk, g_hitch_prog_ns, g_hitch_pipe_n, g_hitch_pipe_ns;   // per frame (hitch log)
static uint64_t g_scache_hits, g_scache_misses;
extern const char *gpu_shader_version();
static const uint32_t SCACHE_MAGIC = 0x31435358;   // "XSC1"
static bool sc_read(FILE *f, void *d, size_t n) { return fread(d, 1, n, f) == n; }
template <class T> static bool sc_vec(FILE *f, std::vector<T> &v) {
    uint32_t n; if (!sc_read(f, &n, 4) || n > (64u << 20) / sizeof(T)) return false;
    v.resize(n); return !n || sc_read(f, v.data(), n * sizeof(T));
}
static void scache_load() {
    if (g_scache_state) return;
    g_scache_state = getenv("XRT_NO_SHADER_CACHE") ? -1 : 1;
    if (g_scache_state < 0) return;
    std::string ver = gpu_shader_version();
    size_t n = 0;
    if (FILE *f = fopen("shader_cache.bin", "rb")) {
        uint32_t magic = 0, vl = 0; std::string fv;
        bool ok = sc_read(f, &magic, 4) && magic == SCACHE_MAGIC && sc_read(f, &vl, 4) && vl < 256;
        if (ok) { fv.resize(vl); ok = sc_read(f, &fv[0], vl) && fv == ver; }
        while (ok) {
            uint64_t key; SCacheEntry e; uint32_t hdr[6];
            if (!sc_read(f, &key, 8)) break;
            if (!sc_vec(f, e.q) || !sc_read(f, hdr, sizeof hdr)) break;
            bool good = true;
            for (int st = 0; st < GS_COUNT && good; st++) good = sc_vec(f, e.desc.spirv[st]);
            if (!good || !sc_vec(f, e.desc.bindings)) break;
            e.desc.num_bindings = hdr[0]; e.desc.vertex_inputs = hdr[1]; e.desc.frag_outputs = hdr[2];
            e.desc.push_constant_stages = hdr[3]; e.desc.ydir_stages = hdr[4]; e.desc.writes_memory = hdr[5] != 0; e.desc.ok = true;
            g_scache[key].push_back(std::move(e)); n++;
        }
        fclose(f);
        if (!ok) { n = 0; g_scache.clear(); LOG(1, "[gpu] shader_cache.bin is from another build: starting a new one"); remove("shader_cache.bin"); }
    }
    bool fresh = !n;
    g_scache_f = fopen("shader_cache.bin", fresh ? "wb" : "ab");
    if (g_scache_f && fresh) { uint32_t vl = (uint32_t)ver.size(); fwrite(&SCACHE_MAGIC, 4, 1, g_scache_f); fwrite(&vl, 4, 1, g_scache_f); fwrite(ver.data(), 1, vl, g_scache_f); fflush(g_scache_f); }
    LOG(1, "[gpu] shader cache: %zu translated programs on disk", n);
}
static void scache_store(uint64_t key, const std::vector<SQuery> &q, const ShaderProgramDesc &d) {
    if (!g_scache_f || !d.ok) return;
    auto wv = [&](const void *p, uint32_t n, size_t sz) { fwrite(&n, 4, 1, g_scache_f); if (n) fwrite(p, sz, n, g_scache_f); };
    fwrite(&key, 8, 1, g_scache_f);
    wv(q.data(), (uint32_t)q.size(), sizeof(SQuery));
    uint32_t hdr[6] = {d.num_bindings, d.vertex_inputs, d.frag_outputs, d.push_constant_stages, d.ydir_stages, d.writes_memory ? 1u : 0u};
    fwrite(hdr, sizeof hdr, 1, g_scache_f);
    for (int st = 0; st < GS_COUNT; st++) wv(d.spirv[st].data(), (uint32_t)d.spirv[st].size(), 4);
    wv(d.bindings.data(), (uint32_t)d.bindings.size(), sizeof(ShaderBinding));
    fflush(g_scache_f);
    g_scache[key].push_back(SCacheEntry{q, d});
}
static uint64_t fnv(uint64_t h, const void *p, size_t n) { const uint8_t *b = (const uint8_t *)p; for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ULL; return h; }
// recording wrappers (the translator's questions and our answers)
static uint32_t qr_read_cbuf(void *u, int stage, uint32_t bank, uint32_t off) {
    uint32_t r = q_read_cbuf(u, stage, bank, off);
    if (g_q_rec) g_q_rec->push_back(SQuery{0, (uint8_t)stage, 0, bank, off, r});
    return r;
}
static int qr_texture_type(void *u, uint32_t h) {
    int r = q_texture_type(u, h);
    if (g_q_rec) g_q_rec->push_back(SQuery{1, 0, 0, h, 0, (uint32_t)r});
    return r;
}
static bool qr_texture_integer(void *u, uint32_t h) {
    bool r = q_texture_integer(u, h);
    if (g_q_rec) g_q_rec->push_back(SQuery{2, 0, 0, h, 0, r ? 1u : 0u});
    return r;
}
// An entry fits when the translator would get the same answers now. Constant-buffer words it read are mostly texture
// handles (bindless): the SPIR-V does not contain them (draws read the handles themselves), only the texture types
// found through them. Most misses were such handles - another material, the same shader and texture types - so a
// word that differs is accepted when the texture-type questions that follow it, asked with today's handle (built
// from today's words the same way: shifts found from the recorded values), get the same answers; any other word
// that differs is a miss. XRT_SCACHE_STRICT=1: every word must be the same (before).
static uint64_t g_scache_hits_subst;
static bool scache_matches(const std::vector<SQuery> &qs, bool *subst_out) {
    static int strict = -1; if (strict < 0) strict = getenv("XRT_SCACHE_STRICT") ? 1 : 0;
    struct Rd { uint32_t rec, cur; bool used; };
    Rd rd[64]; int nrd = 0;                              // (programs ask a few dozen questions at most)
    uint32_t memo_a[32], memo_h[32]; int nmemo = 0;      // recorded handle -> today's handle
    bool subst = false;
    for (const SQuery &q : qs) {
        if (q.kind == 0) {
            uint32_t c = q_read_cbuf(nullptr, q.stage, q.a, q.b);
            if (c != q.r && (strict || nrd >= 64)) return false;
            if (nrd < 64) rd[nrd++] = {q.r, c, c == q.r};   // (a word that is the same needs no explaining)
            continue;
        }
        uint32_t h = q.a; bool have = false;
        for (int i = 0; i < nmemo; i++) if (memo_a[i] == q.a) { h = memo_h[i]; have = true; break; }
        if (!have && nrd > 0) {
            // today's handle from the last one or two words: a == rec1 << s1 | rec0 << s0
            int i1 = nrd - 1, i0 = nrd - 2;
            bool found = false;
            if (i0 >= 0)
                for (int s0 = 0; s0 < 32 && !found; s0++)
                    for (int s1 = 0; s1 < 32 && !found; s1++)
                        if (((rd[i0].rec << s0) | (rd[i1].rec << s1)) == q.a && rd[i0].rec && rd[i1].rec) {
                            h = (rd[i0].cur << s0) | (rd[i1].cur << s1); rd[i0].used = rd[i1].used = true; found = true;
                        }
            for (int k = 0; k < 2 && !found; k++) {
                int i = k == 0 ? i1 : i0; if (i < 0) continue;
                for (int s = 0; s < 32 && !found; s++)
                    if ((rd[i].rec << s) == q.a) {
                        h = rd[i].cur << s; rd[i].used = true; found = true;
                        int j = k == 0 ? i0 : i1;   // the other half of a split handle (sampler bits) that was 0
                        if (j >= 0 && rd[j].rec == 0) { rd[j].used = true; h |= rd[j].cur << 20; }
                    }
            }
            if (found && h != q.a) subst = true;
            if (nmemo < 32) { memo_a[nmemo] = q.a; memo_h[nmemo] = h; nmemo++; }
        }
        uint32_t r = q.kind == 1 ? (uint32_t)q_texture_type(nullptr, h) : (q_texture_integer(nullptr, h) ? 1u : 0u);
        if (r != q.r) return false;
    }
    for (int i = 0; i < nrd; i++) if (!rd[i].used) return false;   // a word that differs and is not a texture handle
    if (subst_out) *subst_out = subst;
    return true;
}

static ProgObj *get_program(const GpuDrawState &s) {
    uint8_t types[32];
    for (int i = 0; i < 32; i++) types[i] = i < s.num_attribs ? attrib_type(s.attribs[i].format) : AT_DISABLED;
    uint8_t kb[sizeof s.shader + 32 + 8 + 2 + 128]; size_t kn = 0;
    memcpy(kb, s.shader, sizeof s.shader); kn += sizeof s.shader;
    memcpy(kb + kn, types, 32); kn += 32;
    kb[kn++] = s.lower_left ? 1 : 0;
    int afunc = alpha_test_func(s); kb[kn++] = (uint8_t)afunc; if (afunc) { memcpy(kb + kn, &s.alpha_ref, 4); kn += 4; }
    int xc = std::min(s.xfb_count, 128); kb[kn++] = (uint8_t)xc; kb[kn++] = (uint8_t)(s.xfb_stride >> 2);
    if (xc) { memcpy(kb + kn, s.xfb_locs, (size_t)xc); kn += (size_t)xc; }
    static const uint8_t *last_key; static size_t last_n; static ProgObj *last_prog;   // most draws repeat the program
    if (last_prog && last_n == kn && !memcmp(last_key, kb, kn)) return last_prog;
    if (std::unique_ptr<ProgObj> *hit = g_progs.find(kb, kn)) {
        last_prog = hit->get(); last_n = kn;
        static std::vector<uint8_t> lk; lk.assign(kb, kb + kn); last_key = lk.data();
        return last_prog;
    }
    struct ProfScope { uint64_t t0 = plat_ticks_ns(); ~ProfScope() { uint64_t d = plat_ticks_ns() - t0; g_hitch_prog_ns += d; gpu_prof_add(PROF_PROG, d); } } prof_scope;
    auto po = std::make_unique<ProgObj>();
    ProgObj *p = po.get();
    g_progs.insert(kb, kn, std::move(po));
    g_q_state = &s;
    ShaderEnvQueries q{nullptr, qr_read_cbuf, qr_texture_type, qr_texture_integer};
    // NVN depth mode "near is -W" (OpenGL clip space): the shader remaps z to Vulkan's 0..1.
    // y_negate (SR_Y_DIRECTION = -1) is for the lower-left origin only, like Eden; the game uses upper-left.
    scache_load();
    uint64_t ckey = 0;
    bool cached = false;
    if (g_scache_state > 0) {   // key: the shader code itself + everything else the translation depends on
        uint64_t h = 0xcbf29ce484222325ULL;
        for (int st = 0; st < GS_COUNT; st++) {
            uint64_t len = s.shader[st] && s.shader_end[st] > s.shader[st] ? (uint64_t)(s.shader_end[st] - s.shader[st]) : 0;
            h = fnv(h, &len, 8); if (len) h = fnv(h, s.shader[st], (size_t)len);
        }
        h = fnv(h, kb + sizeof s.shader, kn - sizeof s.shader);
        uint8_t ex[3] = {(uint8_t)(g_depth_mode == 0), (uint8_t)g_xfb, 0};
        ckey = fnv(h, ex, sizeof ex);
        auto it = g_scache.find(ckey);
        if (it != g_scache.end())
            for (const SCacheEntry &e : it->second) {
                bool subst = false;
                if (scache_matches(e.q, &subst)) { p->desc = e.desc; cached = true; if (subst) g_scache_hits_subst++; break; }
            }
    }
    if (cached) { g_scache_hits++; g_hitch_prog_disk++; }
    else {
        std::vector<SQuery> rec; g_q_rec = &rec;
        bool ok = gpu_translate_program(s.shader, s.shader_end, types, g_depth_mode == 0, false, s.xfb_locs, g_xfb ? s.xfb_count : 0, s.xfb_stride, afunc, s.alpha_ref, q, p->desc);
        g_q_rec = nullptr;
        g_hitch_prog_new++; g_scache_misses++;
        if (!ok) {
            log_once("prog" + p->desc.error, "[gpu] shader translation failed: %s", p->desc.error.c_str());
            return p;
        }
        if (g_scache_state > 0) scache_store(ckey, rec, p->desc);
    }
    if (p->desc.ydir_stages) {
        static int n_ydir; n_ydir++;
        if (n_ydir <= 5 || (n_ydir & (n_ydir - 1)) == 0) LOG(1, "[gpu] %d programs read SR_Y_DIRECTION (stages 0x%x, lower_left %d)", n_ydir, p->desc.ydir_stages, (int)s.lower_left);
    }
    for (int st = 0; st < GS_COUNT; st++) {
        if (p->desc.spirv[st].empty()) continue;
        VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = p->desc.spirv[st].size() * 4; mi.pCode = p->desc.spirv[st].data();
        if (vkCreateShaderModule(g_dev, &mi, nullptr, &p->mod[st]) != VK_SUCCESS) { log_once("mod", "[gpu] vkCreateShaderModule failed"); return p; }
    }
    std::vector<VkDescriptorSetLayoutBinding> lb;
    {   // dynamic uniform buffers when the device allows that many (XRT_NO_DESC_CACHE=1: plain ones, a set per draw)
        uint32_t nu = 0; for (const ShaderBinding &b : p->desc.bindings) if (b.kind == ShaderBinding::UNIFORM) nu += b.count;
        p->dyn_ubo = !desc_cache_off() && nu <= g_limits.maxDescriptorSetUniformBuffersDynamic;
    }
    for (const ShaderBinding &b : p->desc.bindings) {
        VkDescriptorSetLayoutBinding x = {};
        x.binding = b.binding; x.descriptorCount = b.count; x.stageFlags = k_vk_stage[b.stage];
        switch (b.kind) {
        case ShaderBinding::UNIFORM: x.descriptorType = p->dyn_ubo ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; break;
        case ShaderBinding::STORAGE: x.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; break;
        case ShaderBinding::TEXEL_BUFFER: x.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER; break;
        case ShaderBinding::IMAGE_BUFFER: x.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER; break;
        case ShaderBinding::SAMPLED: x.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; break;
        case ShaderBinding::IMAGE: x.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; break;
        }
        lb.push_back(x);
    }
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = (uint32_t)lb.size(); dl.pBindings = lb.data();
    if (vkCreateDescriptorSetLayout(g_dev, &dl, nullptr, &p->dsl) != VK_SUCCESS) return p;
    for (int st = 0; st < GS_COUNT; st++) if (p->desc.push_constant_stages & (1u << st)) p->pc_stages |= k_vk_stage[st];
    VkPushConstantRange pcr = {p->pc_stages, 0, 64};
    VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1; pl.pSetLayouts = &p->dsl;
    pl.pushConstantRangeCount = p->pc_stages ? 1 : 0; pl.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(g_dev, &pl, nullptr, &p->layout) != VK_SUCCESS) return p;
    p->ok = true;
    return p;
}

// ---------------------------------------------------------------- pipelines
struct PipeKey {
    const ProgObj *prog;
    uint32_t topology;
    struct { uint32_t format, offset, stream; } attr[16];
    struct { uint32_t stride, divisor; } stream[16];
    struct { uint32_t enable, src_c, dst_c, src_a, dst_a, eq_c, eq_a, mask; } blend[8];
    uint32_t depth_test, depth_write, depth_func, cull, front, poly;
    uint32_t stencil_test, stencil_func[2], stencil_op[2][3];
    uint32_t color_fmt[8], num_colors, depth_fmt, lower_left, rast_discard;
    uint32_t logic_op, depth_bias;
};
static KeyCache<VkPipeline> g_pipes;
// ---------------------------------------------------------------- asynchronous pipelines (M5: stutter)
// A new pipeline the driver has not compiled before took 50-500 ms on the render worker: the visible stutters of a
// first visit. Pipelines are built on background threads instead. While one is building, a draw that needs it is
// skipped when its pass draws targets that were also drawn the frame before (a per-frame image: one frame without
// that object is barely visible), and waits otherwise (one-shot work such as a baked map must not be lost).
// Off by default (XRT_ASYNC_PIPELINES=1 to try it): in a test the field fell from ~110 to ~15 fps, GPU-bound, a few
// seconds after the first skipped draws, and stayed there.
struct PipeJob {
    VkPipelineShaderStageCreateInfo stages[GS_COUNT]; VkVertexInputAttributeDescription va[16]; VkVertexInputBindingDescription vb[16];
    VkPipelineVertexInputStateCreateInfo vi; VkPipelineInputAssemblyStateCreateInfo ia; VkPipelineViewportStateCreateInfo vp;
    VkPipelineRasterizationStateCreateInfo rs; VkPipelineMultisampleStateCreateInfo ms; VkPipelineDepthStencilStateCreateInfo ds;
    VkPipelineColorBlendAttachmentState ba[8]; VkPipelineColorBlendStateCreateInfo cb; VkDynamicState dyn[7];
    VkPipelineDynamicStateCreateInfo dy; VkFormat cfmt[8]; VkPipelineRenderingCreateInfo ri; VkGraphicsPipelineCreateInfo gi;
    std::atomic<int> state{0};   // 0 queued, 1 done
    VkPipeline result = VK_NULL_HANDLE; uint64_t ns = 0;
};
static std::mutex g_pj_m; static std::condition_variable g_pj_cv;
static std::deque<std::shared_ptr<PipeJob>> g_pj_queue;
static std::unordered_map<VkPipeline *, std::shared_ptr<PipeJob>> g_pj_pending;   // cache slot -> job building it
static std::atomic<uint64_t> g_pj_built_ns{0}, g_pj_built{0};
static uint64_t g_pj_skipped, g_pj_waited;
static void pipe_job_run(PipeJob &j) {
    uint64_t t0 = plat_ticks_ns();
    if (vkCreateGraphicsPipelines(g_dev, pipeline_cache(), 1, &j.gi, nullptr, &j.result) != VK_SUCCESS) j.result = VK_NULL_HANDLE;
    j.ns = plat_ticks_ns() - t0;
    g_pj_built_ns += j.ns; g_pj_built++;
}
static void pipe_worker() {
    xrt_sampler_register_thread("pipeline builder");
    for (;;) {
        std::shared_ptr<PipeJob> j;
        { std::unique_lock<std::mutex> l(g_pj_m); g_pj_cv.wait(l, [] { return !g_pj_queue.empty(); }); j = g_pj_queue.front(); g_pj_queue.pop_front(); }
        pipe_job_run(*j);
        { std::lock_guard<std::mutex> l(g_pj_m); j->state = 1; }
        g_pj_cv.notify_all();
    }
}
static int async_pipes() {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("XRT_ASYNC_PIPELINES"); on = e ? atoi(e) : 0;   // (off: with it on, the field fell to ~15 fps, GPU-bound - not understood yet)
        if (on) { pipeline_cache(); unsigned n = std::max(1u, std::min(4u, std::thread::hardware_concurrency() / 4)); for (unsigned i = 0; i < n; i++) std::thread(pipe_worker).detach();
                  LOG(1, "[gpu] pipelines built on %u background threads (XRT_ASYNC_PIPELINES=1)", n); }
    }
    return on;
}
// the draw only writes images that were drawn the frame before too (and no buffers)
static bool draw_may_skip(const GpuDrawState &s, const ProgObj *p) {
    if (p->desc.writes_memory || s.xfb_count) return false;
    auto per_frame = [](const GpuTex *t) { return t->rt_frame == g_frame ? t->rt_prev_frame + 1 == g_frame : t->rt_frame + 1 == g_frame; };
    int n = 0;
    for (int i = 0; i < s.num_colors && i < 8; i++) if (GpuTex *t = s.color[i]) { n++; if (!per_frame(t)) return false; }
    if (GpuTex *t = s.depth) { n++; if (!per_frame(t)) return false; }
    return n > 0;
}
// a cache slot holding VK_NULL_HANDLE: built (failed), or still building -> finished now / skip / wait
static VkPipeline pipe_pending(VkPipeline *slot, bool may_skip) {
    std::shared_ptr<PipeJob> j;
    { std::lock_guard<std::mutex> l(g_pj_m); auto it = g_pj_pending.find(slot); if (it == g_pj_pending.end()) return *slot; j = it->second; }
    if (!j->state.load()) {
        if (may_skip) { g_pj_skipped++; return VK_NULL_HANDLE; }
        uint64_t t0 = plat_ticks_ns();
        {   // (taken off the queue and built here if no worker has started on it yet)
            std::unique_lock<std::mutex> l(g_pj_m);
            auto q = std::find(g_pj_queue.begin(), g_pj_queue.end(), j);
            if (q != g_pj_queue.end()) { g_pj_queue.erase(q); l.unlock(); pipe_job_run(*j); l.lock(); j->state = 1; g_pj_cv.notify_all(); }
            else g_pj_cv.wait(l, [&] { return j->state.load() == 1; });
        }
        g_pj_waited++;
        g_hitch_pipe_n++; g_hitch_pipe_ns += plat_ticks_ns() - t0;
    }
    if (!j->result) log_once("pipe", "[gpu] vkCreateGraphicsPipelines failed");
    *slot = j->result;
    { std::lock_guard<std::mutex> l(g_pj_m); g_pj_pending.erase(slot); }
    return *slot;
}
static VkBlendFactor blend_factor(int f) {
    switch (f) {   // NVNblendFunc (GL-like values; dual-source at 0x10, constant colour at 0x61)
    case 1: return VK_BLEND_FACTOR_ZERO;                 case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;            case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;            case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;            case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;            case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR;        case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;        case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;    case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;    case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: { static uint32_t seen[8]; if (f >= 0 && f < 256 && !(seen[f >> 5] & (1u << (f & 31)))) { seen[f >> 5] |= 1u << (f & 31); LOG(1, "[gpu] unknown blend factor %#x", f); } return VK_BLEND_FACTOR_ONE; }
    }
}
static VkBlendOp blend_op(int e) {
    static const VkBlendOp m[] = {VK_BLEND_OP_ADD, VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX};
    return e >= 0 && e < 6 ? m[e] : VK_BLEND_OP_ADD;
}
static bool topology(int prim, VkPrimitiveTopology &t) {
    switch (prim) {   // NVNdrawPrimitive (OpenGL order)
    case 0: t = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; return true;
    case 1: t = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; return true;
    case 3: t = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; return true;
    case 4: t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; return true;
    case 5: t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; return true;
    case 6: t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; return true;
    case 7: t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; return true;     // quads: expanded to triangles in gpu_draw
    case 10: t = VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY; return true;
    case 12: t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY; return true;
    default: return false;     // line loops, quads, polygons, patches: not yet
    }
}
static VkFormat vertex_format(int nvn) {
    switch (nvn) {   // RGB formats only exist as vertex formats
    case 25: return VK_FORMAT_R8G8B8_UNORM;   case 26: return VK_FORMAT_R8G8B8_SNORM;
    case 27: return VK_FORMAT_R8G8B8_UINT;    case 28: return VK_FORMAT_R8G8B8_SINT;
    case 29: return VK_FORMAT_R16G16B16_SFLOAT; case 30: return VK_FORMAT_R16G16B16_UNORM;
    case 31: return VK_FORMAT_R16G16B16_SNORM; case 32: return VK_FORMAT_R16G16B16_UINT;
    case 33: return VK_FORMAT_R16G16B16_SINT;  case 34: return VK_FORMAT_R32G32B32_SFLOAT;
    case 35: return VK_FORMAT_R32G32B32_UINT;  case 36: return VK_FORMAT_R32G32B32_SINT;
    }
    const NvnFormat *f = nvn_format(nvn);
    return f && !f->depth ? (VkFormat)f->vk : VK_FORMAT_UNDEFINED;
}
static bool float_format(VkFormat f) {   // logic ops don't apply to float / sRGB attachments
    switch (f) {
    case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return true;
    default: return false;
    }
}
// NVN ColorState logic op (GL order 0 CLEAR .. 15 SET = VkLogicOp; 3 COPY = off). 0 = no logic op in the pipeline,
// else VkLogicOp + 1. Only when every colour target is integer / normalized (Monado T176). XRT_NO_LOGIC_OP=1: off.
static int pipe_logic_op(const GpuDrawState &s) {
    static int off = -1; if (off < 0) off = getenv("XRT_NO_LOGIC_OP") ? 1 : 0;
    if (off || s.logic_op == 3 || s.logic_op < 0 || s.logic_op > 15 || s.num_colors == 0) return 0;
    for (int i = 0; i < s.num_colors && i < 8; i++) if (s.color[i] && float_format(s.color[i]->format)) return 0;
    { static uint32_t seen; if (!(seen & (1u << s.logic_op))) { seen |= 1u << s.logic_op; LOG(1, "[gpu] logic op %d in use", s.logic_op); } }
    return s.logic_op + 1;
}
// Polygon offset applies to filled polygons when the FILL bit (4) is set (lines/points: their bits). XRT_NO_DEPTH_BIAS=1: off.
static uint32_t depth_bias_on(const GpuDrawState &s) {
    static int off = -1; if (off < 0) off = getenv("XRT_NO_DEPTH_BIAS") ? 1 : 0;
    if (off || !s.depth) return 0;
    int bit = s.polygon_mode == 0 ? 1 : s.polygon_mode == 1 ? 2 : 4;
    if (!(s.poly_offset_enables & bit)) return 0;
    { static int n; if (n++ < 4) LOG(1, "[gpu] polygon offset: factor %g units %g clamp %g", s.poly_offset[0], s.poly_offset[1], s.poly_offset[2]); }
    return 1;
}
static VkPipeline get_pipeline(const GpuDrawState &s, ProgObj *p, VkPrimitiveTopology topo) {
    PipeKey k; memset(&k, 0, sizeof k);
    k.prog = p; k.topology = topo;
    for (int i = 0; i < 16; i++) if (p->desc.vertex_inputs & (1u << i) && i < s.num_attribs) {
        k.attr[i] = {(uint32_t)s.attribs[i].format, s.attribs[i].offset, (uint32_t)s.attribs[i].stream};
        int st = s.attribs[i].stream & 15;
        k.stream[st] = {s.streams[st].stride, s.streams[st].divisor};
    }
    k.num_colors = (uint32_t)s.num_colors;
    for (int i = 0; i < s.num_colors && i < 8; i++) {
        k.color_fmt[i] = s.color[i] ? (uint32_t)s.color[i]->format : 0;
        const GpuBlend &b = s.blend[i];
        k.blend[i] = {(uint32_t)((s.blend_enable >> i) & 1), (uint32_t)b.src_c, (uint32_t)b.dst_c, (uint32_t)b.src_a, (uint32_t)b.dst_a, (uint32_t)b.eq_c, (uint32_t)b.eq_a, s.channel_mask[i]};
    }
    k.depth_fmt = s.depth ? (uint32_t)s.depth->format : 0;
    k.depth_test = s.depth && s.depth_test; k.depth_write = s.depth && s.depth_write; k.depth_func = (uint32_t)s.depth_func;
    k.stencil_test = s.depth && s.depth->depth_kind >= 2 && s.stencil_test;
    if (k.stencil_test) for (int f = 0; f < 2; f++) { k.stencil_func[f] = (uint32_t)s.stencil_func[f]; for (int j = 0; j < 3; j++) k.stencil_op[f][j] = (uint32_t)s.stencil_op[f][j]; }
    k.cull = (uint32_t)s.cull_face; k.front = (uint32_t)s.front_face; k.poly = (uint32_t)s.polygon_mode; k.lower_left = s.lower_left; k.rast_discard = s.rast_discard;
    k.logic_op = (uint32_t)pipe_logic_op(s); k.depth_bias = depth_bias_on(s);
    static PipeKey last_k; static VkPipeline last_pipe = VK_NULL_HANDLE;   // most draws repeat the previous pipeline
    if (last_pipe && !memcmp(&last_k, &k, sizeof k)) return last_pipe;
    if (VkPipeline *hit = g_pipes.find(&k, sizeof k)) {
        VkPipeline pp = *hit ? *hit : pipe_pending(hit, draw_may_skip(s, p));
        if (pp) { last_k = k; last_pipe = pp; }
        return pp;
    }

    auto job = std::make_shared<PipeJob>();
    auto &stages = job->stages; uint32_t ns = 0;
    for (int st = 0; st < GS_COUNT; st++) if (p->mod[st]) {
        stages[ns] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[ns].stage = k_vk_stage[st]; stages[ns].module = p->mod[st]; stages[ns].pName = "main"; ns++;
    }
    auto &va = job->va; uint32_t nva = 0;
    auto &vb = job->vb; uint32_t nvb = 0; bool have_stream[16] = {};
    for (int i = 0; i < 16; i++) {
        if (!(p->desc.vertex_inputs & (1u << i)) || i >= s.num_attribs) continue;
        VkFormat f = vertex_format(s.attribs[i].format);
        if (f == VK_FORMAT_UNDEFINED) { log_once("vfmt" + std::to_string(s.attribs[i].format), "[gpu] unknown vertex format %d", s.attribs[i].format); continue; }
        int st = s.attribs[i].stream & 15;
        va[nva++] = {(uint32_t)i, (uint32_t)st, f, s.attribs[i].offset};
        if (!have_stream[st]) {
            have_stream[st] = true;
            vb[nvb++] = {(uint32_t)st, s.streams[st].stride, s.streams[st].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        }
    }
    auto &vi = job->vi; vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = nvb; vi.pVertexBindingDescriptions = vb;
    vi.vertexAttributeDescriptionCount = nva; vi.pVertexAttributeDescriptions = va;
    auto &ia = job->ia; ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = topo;
    auto &vp = job->vp; vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;
    auto &rs = job->rs; rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = s.polygon_mode == 0 ? VK_POLYGON_MODE_POINT : s.polygon_mode == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = (VkCullModeFlags)(s.cull_face & 3);       // NONE, FRONT, BACK, FRONT_AND_BACK
    rs.rasterizerDiscardEnable = s.rast_discard ? VK_TRUE : VK_FALSE;
    // The lower-left origin is emulated with a negative-height viewport. Vulkan determines facing after the
    // viewport transform, so the flip already gives OpenGL's winding: NVN's front face maps over unchanged.
    // (Verified on the title screen: inverting it here culled the front faces of every back-face-culled mesh.)
    bool ccw = s.front_face != 0;
    rs.frontFace = ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;
    rs.depthBiasEnable = k.depth_bias ? VK_TRUE : VK_FALSE;
    static int nocull = getenv("XRT_DEBUG_NOCULL") ? 1 : 0;
    if (nocull) rs.cullMode = VK_CULL_MODE_NONE;
    auto &ms = job->ms; ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    auto &ds = job->ds; ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = k.depth_test; ds.depthWriteEnable = k.depth_write;
    ds.depthCompareOp = (VkCompareOp)std::max(0, std::min(7, s.depth_func - 1));
    if (k.stencil_test) {
        ds.stencilTestEnable = VK_TRUE;
        auto sop = [](uint32_t o) { return (VkStencilOp)std::max(0, std::min(7, (int)o - 1)); };   // NVN 1 KEEP .. 8 DECR_WRAP
        VkStencilOpState *st[2] = {&ds.front, &ds.back};
        for (int f = 0; f < 2; f++) {
            st[f]->compareOp = (VkCompareOp)std::max(0, std::min(7, (int)k.stencil_func[f] - 1));
            st[f]->failOp = sop(k.stencil_op[f][0]); st[f]->depthFailOp = sop(k.stencil_op[f][1]); st[f]->passOp = sop(k.stencil_op[f][2]);
            st[f]->compareMask = 0xff; st[f]->writeMask = 0xff; st[f]->reference = 0;   // dynamic
        }
        // front/back stay as NVN defines them (see the winding note above)
    }
    auto &ba = job->ba;
    for (int i = 0; i < s.num_colors && i < 8; i++) {
        const GpuBlend &b = s.blend[i];
        ba[i] = {};
        ba[i].blendEnable = (s.blend_enable >> i) & 1;
        ba[i].srcColorBlendFactor = blend_factor(b.src_c); ba[i].dstColorBlendFactor = blend_factor(b.dst_c);
        ba[i].colorBlendOp = blend_op(b.eq_c);
        ba[i].srcAlphaBlendFactor = blend_factor(b.src_a); ba[i].dstAlphaBlendFactor = blend_factor(b.dst_a);
        ba[i].alphaBlendOp = blend_op(b.eq_a);
        ba[i].colorWriteMask = s.channel_mask[i] & 15;
        static int noblend = getenv("XRT_DEBUG_NOBLEND") ? 1 : 0;
        if (noblend) { ba[i].blendEnable = VK_FALSE; ba[i].colorWriteMask = 15; }
    }
    auto &cb = job->cb; cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = (uint32_t)std::min(s.num_colors, 8); cb.pAttachments = ba;
    if (k.logic_op) { cb.logicOpEnable = VK_TRUE; cb.logicOp = (VkLogicOp)(k.logic_op - 1); }
    auto &dyn = job->dyn;
    { const VkDynamicState d0[7] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                    VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                    VK_DYNAMIC_STATE_DEPTH_BIAS}; memcpy(dyn, d0, sizeof d0); }
    auto &dy = job->dy; dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = k.depth_bias ? 7 : 6; dy.pDynamicStates = dyn;
    auto &cfmt = job->cfmt;
    for (int i = 0; i < 8; i++) cfmt[i] = i < s.num_colors && s.color[i] ? s.color[i]->format : VK_FORMAT_UNDEFINED;
    auto &ri = job->ri; ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = cb.attachmentCount; ri.pColorAttachmentFormats = cfmt;
    ri.depthAttachmentFormat = s.depth ? s.depth->format : VK_FORMAT_UNDEFINED;
    ri.stencilAttachmentFormat = s.depth && s.depth->depth_kind >= 2 ? s.depth->format : VK_FORMAT_UNDEFINED;
    auto &gi = job->gi; gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &ri;
    gi.stageCount = ns; gi.pStages = stages; gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp; gi.pRasterizationState = &rs; gi.pMultisampleState = &ms; gi.pDepthStencilState = &ds;
    gi.pColorBlendState = &cb; gi.pDynamicState = &dy; gi.layout = p->layout;
    g_pcache_new++;
    if (async_pipes()) {   // built in the background; this draw: skipped or waited for (pipe_pending)
        VkPipeline *slot = &g_pipes.insert(&k, sizeof k, VK_NULL_HANDLE);
        { std::lock_guard<std::mutex> l(g_pj_m); g_pj_pending[slot] = job; g_pj_queue.push_back(job); }
        g_pj_cv.notify_all();
        return pipe_pending(slot, draw_may_skip(s, p));
    }
    VkPipeline pipe = VK_NULL_HANDLE;
    uint64_t prof_tp = plat_ticks_ns();
    VkResult pr = vkCreateGraphicsPipelines(g_dev, pipeline_cache(), 1, &gi, nullptr, &pipe);
    gpu_prof_add(PROF_PIPE, plat_ticks_ns() - prof_tp);
    g_hitch_pipe_n++; g_hitch_pipe_ns += plat_ticks_ns() - prof_tp;
    if (pr != VK_SUCCESS) {
        log_once("pipe", "[gpu] vkCreateGraphicsPipelines failed");
        pipe = VK_NULL_HANDLE;
    }
    if (pipe) { last_k = k; last_pipe = pipe; }
    return g_pipes.insert(&k, sizeof k, pipe);
}

// ---------------------------------------------------------------- render passes
static struct { bool active, prof; GpuTex *color[8]; int layer[8]; int level[8]; int n; GpuTex *depth; int dlayer, dlevel; VkExtent2D ext; } g_pass;
static uint64_t g_flushes;
// The frame's commands are recorded as the game submits them but run on the GPU at present. The game
// fences GPU work and reuses (rewrites) buffers once nvnSyncWait says the GPU is done with them: so a wait
// must really execute everything recorded so far, or the GPU later reads already-overwritten vertex data.
void gpu_flush() {
    if (!gpu_available() || !g_recording) return;
    gpu_end_pass();
    vkEndCommandBuffer(g_cmd); g_recording = false;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &g_cmd;
    submit_frame(&si, g_fence);
    vkWaitForFences(g_dev, 1, &g_fence, VK_TRUE, UINT64_MAX); vkResetFences(g_dev, 1, &g_fence); vkResetCommandBuffer(g_cmd, 0);
    g_flushes++;
}
// Mid-frame submit (milestone M5): the frame's commands so far go to the GPU now, without waiting, and recording
// continues in another command buffer of the same frame slot - the GPU runs the first passes while the rest of the
// frame is still being translated, instead of starting only at present. Submission order covers the ordering (the
// barrier at the end, and the slot's fence at present also covering these submits).
static uint64_t g_kicks;
void gpu_kick() {
    if (!gpu_available() || !g_recording) return;
    if (g_pending_scan) {   // the write-watch scan is still running: keep recording, submit at a later boundary
        std::lock_guard<std::mutex> l(g_scan_m);
        if (g_scan_done < g_pending_scan) return;
    }
    gpu_end_pass(); full_barrier();
    vkEndCommandBuffer(g_cmd); g_recording = false;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &g_cmd;
    submit_frame(&si, VK_NULL_HANDLE);
    FrameSlot &fs = g_slots[g_slot];
    if (fs.extra_used == fs.extra.size()) {
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = g_pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(g_dev, &ai, &cb) != VK_SUCCESS) { gpu_flush(); return; }   // (fallback: wait, keep the buffer)
        fs.extra.push_back(cb);
    }
    g_cmd = fs.extra[fs.extra_used++];
    g_kicks++;
}
static uint64_t g_game_barriers, g_memw_barriers;
void gpu_end_pass() {
    if (!g_pass.active) return;
    pfnCmdEndRendering(g_cmd);
    if (g_pass.prof) { vkCmdWriteTimestamp(g_cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_ppq, (uint32_t)g_pp_frame.size() * 2 - 1); g_pass.prof = false; }
    g_pass.active = false;
    full_barrier();
}
// XRT_DUMP_RT=frame: save every render target drawn into during that frame (frames/rt_*.ppm + rt_list.txt)
static long g_dump_rt = -2;
static std::vector<std::pair<GpuTex *, uint64_t>> g_frame_rts;     // target, draws into it
static void note_rt(GpuTex *t) {
    if (g_dump_rt == -2) { const char *e = getenv("XRT_DUMP_RT"); g_dump_rt = e ? atol(e) : -1; }
    if (g_dump_rt < 0 || (long)g_frame + 1 != g_dump_rt || !t) return;
    for (auto &p : g_frame_rts) if (p.first == t) return;
    g_frame_rts.push_back({t, 0});
}
static void count_rt_draw() {
    if (g_frame_rts.empty()) return;
    for (int i = 0; i < 8; i++) for (auto &p : g_frame_rts) if (p.first == g_pass.color[i]) p.second++;
    for (auto &p : g_frame_rts) if (p.first == g_pass.depth) p.second++;
}
// XRT_SNAP=frame,i,j,k...: after draw i of that frame, copy colour target 0 (raw texels) -> frames/snap_<i>_*.bin
struct Snap { long draw; int w, h, fmt; VkBuffer buf; VkDeviceMemory mem; void *map; };
static std::vector<Snap> g_snaps;
static bool debug_needs_sync() { return !g_frame_rts.empty() || !g_snaps.empty() || (g_dump_rt > 0 && (long)g_frame >= g_dump_rt - 1 && (long)g_frame <= g_dump_rt + 1); }
static void snap_after_draw(long frame, long draw_idx, GpuTex *t) {
    static long sframe = -2; static std::vector<long> sdraws;
    if (sframe == -2) {
        sframe = -1;
        if (const char *e = getenv("XRT_SNAP")) {
            sframe = atol(e);
            for (const char *p = strchr(e, ','); p; p = strchr(p + 1, ',')) sdraws.push_back(atol(p + 1));
        }
    }
    if (frame != sframe || !t || t->depth_kind || t->depth3) return;
    if (std::find(sdraws.begin(), sdraws.end(), draw_idx) == sdraws.end()) return;
    Snap sn{draw_idx, t->w, t->h, (int)t->format, VK_NULL_HANDLE, VK_NULL_HANDLE, nullptr};
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = (VkDeviceSize)t->w * t->h * 16; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(g_dev, &bi, nullptr, &sn.buf) != VK_SUCCESS) return;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_dev, sn.buf, &mr);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size; mai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(g_dev, &mai, nullptr, &sn.mem) != VK_SUCCESS) { vkDestroyBuffer(g_dev, sn.buf, nullptr); return; }
    vkBindBufferMemory(g_dev, sn.buf, sn.mem, 0);
    vkMapMemory(g_dev, sn.mem, 0, VK_WHOLE_SIZE, 0, &sn.map);
    gpu_end_pass();
    VkBufferImageCopy bc = {}; bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bc.imageExtent = {(uint32_t)t->w, (uint32_t)t->h, 1};
    vkCmdCopyImageToBuffer(g_cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, sn.buf, 1, &bc);
    full_barrier();
    g_snaps.push_back(sn);
}
static void write_snaps() {
    for (Snap &sn : g_snaps) {
        int bpp = sn.fmt == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : sn.fmt == VK_FORMAT_R32G32B32A32_SFLOAT ? 16 : sn.fmt == VK_FORMAT_R16_SFLOAT ? 2 : 4;
        char name[96]; snprintf(name, sizeof name, "frames/snap_%04ld_%dx%d_f%d.bin", sn.draw, sn.w, sn.h, sn.fmt);
        if (FILE *f = fopen(name, "wb")) { fwrite(sn.map, (size_t)bpp, (size_t)sn.w * sn.h, f); fclose(f); }
        vkDestroyBuffer(g_dev, sn.buf, nullptr); vkFreeMemory(g_dev, sn.mem, nullptr);
    }
    if (!g_snaps.empty()) LOG(0, "[gpu] wrote %zu draw snapshots", g_snaps.size());
    g_snaps.clear();
}
static std::string g_dl; static long g_dl_idx; static FILE *g_dl_file;
// F12 in the window: the next frame gets the XRT_DUMP_RT treatment (render targets, raw dumps, draw_list.txt)
static std::map<GpuTex *, std::vector<uint8_t>> g_dd_prev;   // F12 per-draw diff (see dd_after)
static void capture_next_frame() {
#ifdef _WIN32
    _mkdir("frames");
#else
    mkdir("frames", 0755);
#endif
    if (g_dl_file) { fclose(g_dl_file); g_dl_file = nullptr; }
    g_dump_rt = (long)g_frame + 1;
    g_dd_prev.clear();
    LOG(0, "[gpu] F12: capturing frame %ld (render targets + draw list in frames/)", g_dump_rt);
}
static void dump_frame_rts() {
    write_snaps();
    if (g_frame_rts.empty()) return;
    FILE *list = fopen("frames/rt_list.txt", "w");
    int i = 0;
    for (auto &p : g_frame_rts) {
        GpuTex *t = p.first;
        if (list) fprintf(list, "rt %02d: %dx%d layers %d vkformat %d depth %d draws %llu\n", i, t->w, t->h, t->layers, (int)t->format, t->depth_kind, (unsigned long long)p.second);
        if (t->depth_kind && t->layers == 1 && ensure_readback(t->w, t->h)) {
            // depth: raw depth-aspect copy (D24: low 24 bits of each u32, D32F: float) -> frames/rt_*_depth.bin
            begin_frame();
            full_barrier();
            VkBufferImageCopy bc = {}; bc.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
            bc.imageExtent = {(uint32_t)t->w, (uint32_t)t->h, 1};
            vkCmdCopyImageToBuffer(g_cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, g_rb_buf, 1, &bc);
            vkEndCommandBuffer(g_cmd); g_recording = false;
            VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &g_cmd;
            submit_frame(&si, g_fence);
            vkWaitForFences(g_dev, 1, &g_fence, VK_TRUE, UINT64_MAX); vkResetFences(g_dev, 1, &g_fence); vkResetCommandBuffer(g_cmd, 0);
            char name[96]; snprintf(name, sizeof name, "frames/rt_%05llu_%02d_%dx%d_f%d_depth.bin", (unsigned long long)g_frame, i, t->w, t->h, (int)t->format);
            if (FILE *f = fopen(name, "wb")) { fwrite(g_rb_map, 4, (size_t)t->w * t->h, f); fclose(f); }
        }
        // raw colour data (for HDR / float targets): frames/rt_*_raw.bin, exact texel bytes of level 0 layer 0
        int bpp = 0;
        switch (t->format) {
        case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R8G8_UNORM: bpp = 2; break;
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: bpp = 4; break;
        case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32_SFLOAT: bpp = 8; break;
        case VK_FORMAT_R32G32B32A32_SFLOAT: bpp = 16; break;
        default: break;
        }
        if (!t->depth_kind && bpp && t->depth3 == 0 && ensure_readback(t->w * 4, t->h)) {   // buffer = (4w) * h * 4 bytes >= w*h*16
            begin_frame();
            full_barrier();
            VkBufferImageCopy bc = {}; bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            bc.imageExtent = {(uint32_t)t->w, (uint32_t)t->h, 1};
            vkCmdCopyImageToBuffer(g_cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, g_rb_buf, 1, &bc);
            vkEndCommandBuffer(g_cmd); g_recording = false;
            VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &g_cmd;
            submit_frame(&si, g_fence);
            vkWaitForFences(g_dev, 1, &g_fence, VK_TRUE, UINT64_MAX); vkResetFences(g_dev, 1, &g_fence); vkResetCommandBuffer(g_cmd, 0);
            char name[96]; snprintf(name, sizeof name, "frames/rt_%05llu_%02d_%dx%d_f%d_raw.bin", (unsigned long long)g_frame, i, t->w, t->h, (int)t->format);
            if (FILE *f = fopen(name, "wb")) { fwrite(g_rb_map, (size_t)bpp, (size_t)t->w * t->h, f); fclose(f); }
        }
        if (!t->depth_kind && format_ok(t->format, VK_FORMAT_FEATURE_BLIT_SRC_BIT) && ensure_readback(t->w, t->h)) {
            begin_frame();
            full_barrier();
            image_barrier(g_rb_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            blit_full(t, g_rb_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, t->w, t->h);
            image_barrier(g_rb_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy bc = {}; bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            bc.imageExtent = {(uint32_t)t->w, (uint32_t)t->h, 1};
            vkCmdCopyImageToBuffer(g_cmd, g_rb_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_rb_buf, 1, &bc);
            vkEndCommandBuffer(g_cmd); g_recording = false;
            VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &g_cmd;
            submit_frame(&si, g_fence);
            vkWaitForFences(g_dev, 1, &g_fence, VK_TRUE, UINT64_MAX); vkResetFences(g_dev, 1, &g_fence); vkResetCommandBuffer(g_cmd, 0);
            char name[96]; snprintf(name, sizeof name, "frames/rt_%05llu_%02d_%dx%d_f%d.ppm", (unsigned long long)g_frame, i, t->w, t->h, (int)t->format);
            if (FILE *f = fopen(name, "wb")) {
                fprintf(f, "P6\n%d %d\n255\n", g_rb_w, g_rb_h);
                const uint8_t *px = (const uint8_t *)g_rb_map; std::vector<uint8_t> row((size_t)g_rb_w * 3);
                for (int y = 0; y < g_rb_h; y++) { for (int x = 0; x < g_rb_w; x++) memcpy(&row[x * 3], px + ((size_t)y * g_rb_w + x) * 4, 3); fwrite(row.data(), 1, row.size(), f); }
                fclose(f);
            }
        }
        i++;
    }
    if (list) fclose(list);
    LOG(0, "[gpu] dumped %zu render targets of frame %llu to frames/", g_frame_rts.size(), (unsigned long long)g_frame);
    g_frame_rts.clear();
}


static bool rt_frame(); static int frame_rt_index(const GpuTex *t);
// raw texel bytes of level 0 layer 0 (small textures only) into g_rb_map
static bool readback_rgba8_raw(GpuTex *t) {
    if (!t || t->depth3 || t->depth_kind || !ensure_readback(t->w * 4, t->h)) return false;
    gpu_end_pass();
    begin_frame(); full_barrier();
    VkBufferImageCopy bc = {}; bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bc.imageExtent = {(uint32_t)t->w, (uint32_t)t->h, 1};
    vkCmdCopyImageToBuffer(g_cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, g_rb_buf, 1, &bc);
    vkEndCommandBuffer(g_cmd); g_recording = false;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &g_cmd;
    submit_frame(&si, g_fence);
    vkWaitForFences(g_dev, 1, &g_fence, VK_TRUE, UINT64_MAX); vkResetFences(g_dev, 1, &g_fence); vkResetCommandBuffer(g_cmd, 0);
    begin_frame();
    return true;
}
// ---- F12 per-draw diff of the main scene buffer (which draw paints what): frames/draw_diff.txt + dd_<draw>.ppm
static bool readback_rgba8(GpuTex *t) {
    if (!t || t->depth_kind || t->depth3 || !format_ok(t->format, VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !ensure_readback(t->w, t->h)) return false;
    gpu_end_pass();
    begin_frame(); full_barrier();
    image_barrier(g_rb_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    blit_full(t, g_rb_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, t->w, t->h);
    image_barrier(g_rb_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy bc = {}; bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bc.imageExtent = {(uint32_t)t->w, (uint32_t)t->h, 1};
    vkCmdCopyImageToBuffer(g_cmd, g_rb_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_rb_buf, 1, &bc);
    vkEndCommandBuffer(g_cmd); g_recording = false;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &g_cmd;
    submit_frame(&si, g_fence);
    vkWaitForFences(g_dev, 1, &g_fence, VK_TRUE, UINT64_MAX); vkResetFences(g_dev, 1, &g_fence); vkResetCommandBuffer(g_cmd, 0);
    begin_frame();   // the draw that follows records into the command buffer again
    return true;
}
static bool dd_wanted(const GpuDrawState &s) {
    static int on = -1; if (on < 0) on = getenv("XRT_NO_DRAW_DIFF") ? 0 : 1;
    static int all = -1; if (all < 0) all = getenv("XRT_DD_ALL") ? 1 : 0;   // also opaque draws (G-buffer)
    return on && rt_frame() && s.num_colors > 0 && s.color[0] && !s.color[0]->depth3 && s.color[0]->w >= 480 && (all || (s.blend_enable & 1));
}
static void dd_before(const GpuDrawState &s) {
    GpuTex *t = s.color[0];
    if (g_dd_prev.count(t) || !readback_rgba8(t)) return;
    g_dd_prev[t].assign((const uint8_t *)g_rb_map, (const uint8_t *)g_rb_map + (size_t)t->w * t->h * 4);
}
static void dd_after(const GpuDrawState &s, long draw_idx) {
    GpuTex *t = s.color[0];
    auto it = g_dd_prev.find(t);
    if (it == g_dd_prev.end() || !readback_rgba8(t)) return;
    const uint8_t *now = (const uint8_t *)g_rb_map; std::vector<uint8_t> &prev = it->second;
    long changed = 0; int x0 = t->w, y0 = t->h, x1 = -1, y1 = -1;
    for (int y = 0; y < t->h; y++) for (int x = 0; x < t->w; x++) {
        size_t o = ((size_t)y * t->w + x) * 4;
        if (memcmp(now + o, prev.data() + o, 3)) { changed++; x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y); }
    }
    char ddn[64]; snprintf(ddn, sizeof ddn, "frames/draw_diff_%05llu.txt", (unsigned long long)g_frame + 1);
    if (FILE *f = fopen(ddn, "a")) {
        fprintf(f, "draw %ld rt%d %dx%d changed %ld px bbox %d,%d-%d,%d prog %llx\n", draw_idx, frame_rt_index(t), t->w, t->h, changed, x0, y0, x1, y1, (unsigned long long)s.program);
        fclose(f);
    }
    if (changed > 0) {   // half-size thumbnail of the target after this draw
        char name[64]; snprintf(name, sizeof name, "frames/dd_%05llu_%04ld.ppm", (unsigned long long)g_frame + 1, draw_idx);
        if (FILE *f = fopen(name, "wb")) {
            int w = t->w / 2, h = t->h / 2;
            fprintf(f, "P6\n%d %d\n255\n", w, h);
            std::vector<uint8_t> row((size_t)w * 3);
            for (int y = 0; y < h; y++) { for (int x = 0; x < w; x++) memcpy(&row[x * 3], now + ((size_t)(2 * y) * t->w + 2 * x) * 4, 3); fwrite(row.data(), 1, row.size(), f); }
            fclose(f);
        }
    }
    prev.assign(now, now + (size_t)t->w * t->h * 4);
}
static bool begin_pass(const GpuDrawState &s) {
    int n = std::min(s.num_colors, 8);
    bool same = g_pass.active && g_pass.n == n && g_pass.depth == s.depth && g_pass.dlayer == s.depth_layer && g_pass.dlevel == s.depth_level;
    for (int i = 0; same && i < n; i++) same = g_pass.color[i] == s.color[i] && g_pass.layer[i] == s.color_layer[i] && g_pass.level[i] == s.color_level[i];
    if (same) return true;
    gpu_end_pass();
    VkRenderingAttachmentInfo ca[8]; uint32_t w = UINT32_MAX, h = UINT32_MAX; bool any = false;
    for (int i = 0; i < n; i++) {
        ca[i] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (s.color[i]) {
            ca[i].imageView = rt_view(s.color[i], s.color_layer[i], s.color_level[i]); ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            int lv = s.color[i]->depth3 ? 0 : s.color_level[i];
            w = std::min(w, mip_dim(s.color[i]->w, lv)); h = std::min(h, mip_dim(s.color[i]->h, lv)); any = true;
        }
    }
    VkRenderingAttachmentInfo da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (s.depth) {
        da.imageView = rt_view(s.depth, s.depth_layer, s.depth_level); da.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        w = std::min(w, mip_dim(s.depth->w, s.depth_level)); h = std::min(h, mip_dim(s.depth->h, s.depth_level)); any = true;
    }
    if (!any) return false;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {w, h}}; ri.layerCount = 1;
    ri.colorAttachmentCount = (uint32_t)n; ri.pColorAttachments = ca;
    ri.pDepthAttachment = s.depth ? &da : nullptr;
    ri.pStencilAttachment = s.depth && s.depth->depth_kind >= 2 ? &da : nullptr;
    if (g_ppq && g_pp_frame.size() < PP_MAX) {
        char k[160]; int m = snprintf(k, sizeof k, "%ux%u", w, h);
        for (int i = 0; i < n; i++) m += snprintf(k + m, sizeof k - m, " c%d", s.color[i] ? (int)s.color[i]->format : 0);
        if (s.depth) snprintf(k + m, sizeof k - m, " d%d", (int)s.depth->format);
        vkCmdWriteTimestamp(g_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_ppq, (uint32_t)g_pp_frame.size() * 2);
        g_pp_frame.push_back(PassRec{k, 0});
        g_pass.prof = true;
    }
    pfnCmdBeginRendering(g_cmd, &ri);
    g_cnt_pass++;
    g_pass.active = true; g_pass.n = n; g_pass.depth = s.depth; g_pass.dlayer = s.depth_layer; g_pass.dlevel = s.depth_level; g_pass.ext = {w, h};
    for (int i = 0; i < n; i++) note_rt(s.color[i]);
    note_rt(s.depth);
    auto mark = [](GpuTex *t) { if (t && t->rt_frame != g_frame) { t->rt_prev_frame = t->rt_frame; t->rt_frame = g_frame; } };
    for (int i = 0; i < n; i++) mark(s.color[i]);
    mark(s.depth);
    for (int i = 0; i < n; i++) { g_pass.color[i] = s.color[i]; g_pass.layer[i] = s.color_layer[i]; g_pass.level[i] = s.color_level[i]; }
    return true;
}

// ---------------------------------------------------------------- draw
static uint64_t g_draw_ok, g_draw_skip;
static uint64_t g_skip_why[8];   // 0 no shader, 1 prim, 2 program, 3 set, 4 vb, 5 ib, 6 pipeline, 7 pass
// XRT_DUMP_RT frame: also write frames/draw_list.txt (one line per draw: targets, program, textures, result)
static bool rt_frame() {
    if (g_dump_rt == -2) { const char *e = getenv("XRT_DUMP_RT"); g_dump_rt = e ? atol(e) : -1; }
    return g_dump_rt >= 0 && (long)g_frame + 1 == g_dump_rt;
}
static int frame_rt_index(const GpuTex *t) {
    for (size_t i = 0; i < g_frame_rts.size(); i++) if (g_frame_rts[i].first == t) return (int)i;
    return -1;
}
static void dl_flush(const char *result) {
    if (g_dl.empty()) return;
    if (!g_dl_file) { char n[64]; snprintf(n, sizeof n, "frames/draw_list_%05llu.txt", (unsigned long long)g_frame + 1); g_dl_file = fopen(n, "w"); }
    if (g_dl_file) { fprintf(g_dl_file, "%s -> %s\n", g_dl.c_str(), result); fflush(g_dl_file); }
    g_dl.clear();
}
static void dl_add(const char *fmt, ...) {
    if (g_dl.empty()) return;
    char b[256]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); g_dl += b;
}
// nvnCommandBufferBarrier(bits): ORDER_PRIMITIVES 1, ORDER_FRAGMENTS 2, ORDER_FRAGMENTS_TILED 4, ORDER_INDIRECT_DATA 8,
// INVALIDATE_TEXTURE 0x10, INVALIDATE_SHADER 0x20, INVALIDATE_TEXTURE_DESCRIPTOR 0x40, INVALIDATE_ZCULL 0x80,
// INVALIDATE_L2_CACHE 0x100. Anything but a pure z-cull invalidate orders work: end the render pass (a full barrier).
// XRT_NO_BARRIER=1: ignore them (the old behaviour).
void gpu_barrier(uint32_t bits) {
    static int off = -1; if (off < 0) off = getenv("XRT_NO_BARRIER") ? 1 : 0;
    if (off || !(bits & ~0x80u)) return;
    g_game_barriers++;
    if (g_dl_file) { fprintf(g_dl_file, "barrier %x%s\n", bits, g_pass.active ? " (ends pass)" : ""); fflush(g_dl_file); }
    gpu_end_pass();
}
// texel buffer views over the imported pool memory, cached by (address, size, format)
static VkBufferView texel_view(uint32_t id) {
    uint64_t addr, size; int fmt;
    if (!g_res.texture_buffer || !g_res.texture_buffer(g_res.user, id, &addr, &size, &fmt) || !size) return VK_NULL_HANDLE;
    static std::map<std::tuple<uint64_t, uint64_t, int>, VkBufferView> cache;
    auto key = std::make_tuple(addr, size, fmt);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    VkBufferView v = VK_NULL_HANDLE;
    const NvnFormat *f = nvn_format(fmt);
    VkBuffer buf; VkDeviceSize off;
    VkDeviceSize al = g_limits.minTexelBufferOffsetAlignment ? g_limits.minTexelBufferOffsetAlignment : 16;
    if (f && f->vk && lookup_buffer(addr, size, buf, off)) {
        if (off % al) log_once("tbalign", "[gpu] texel buffer at %llx is not %llu-byte aligned", (unsigned long long)addr, (unsigned long long)al);
        else {
            VkBufferViewCreateInfo bv = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
            bv.buffer = buf; bv.format = (VkFormat)f->vk; bv.offset = off;
            uint64_t maxb = (uint64_t)g_limits.maxTexelBufferElements * (uint64_t)f->bpb;
            bv.range = std::min<uint64_t>(size, maxb);
            if (vkCreateBufferView(g_dev, &bv, nullptr, &v) != VK_SUCCESS) { v = VK_NULL_HANDLE; log_once("tbview", "[gpu] vkCreateBufferView failed (format %d)", fmt); }
            else { static int n; if (n++ < 8) LOG(1, "[gpu] texel buffer %#x: %llx, %llu bytes, %s", id, (unsigned long long)addr, (unsigned long long)size, f->name); }
        }
    } else log_once("tbmiss" + std::to_string(fmt), "[gpu] texel buffer %#x unresolved (format %d, addr %llx)", id, fmt, (unsigned long long)addr);
    cache[key] = v;
    return v;
}
static const char *const g_skip_names[8] = {"SKIP no-vertex-shader", "SKIP prim", "SKIP program", "SKIP set", "SKIP vb", "SKIP ib", "SKIP pipeline", "SKIP pass"};
#define SKIP(n) do { g_draw_skip++; g_skip_why[n]++; dl_flush(g_skip_names[n]); } while (0)
void gpu_draw_stats() {
    LOG(1, "[gpu] %llu mid-frame submits, %llu sync flushes, %llu game barriers, %llu memory-write barriers", (unsigned long long)g_kicks, (unsigned long long)g_flushes, (unsigned long long)g_game_barriers, (unsigned long long)g_memw_barriers);
    LOG(1, "[gpu] draws: %llu executed, %llu skipped (shader %llu, prim %llu, program %llu, set %llu, vb %llu, ib %llu, pipeline %llu, pass %llu); %zu programs, %zu pipelines",
        (unsigned long long)g_draw_ok, (unsigned long long)g_draw_skip, (unsigned long long)g_skip_why[0], (unsigned long long)g_skip_why[1],
        (unsigned long long)g_skip_why[2], (unsigned long long)g_skip_why[3], (unsigned long long)g_skip_why[4], (unsigned long long)g_skip_why[5],
        (unsigned long long)g_skip_why[6], (unsigned long long)g_skip_why[7], g_progs.size(), g_pipes.size());
}
static void gpu_draw_impl(const GpuDrawState &s, const GpuDraw &d);
void gpu_draw(const GpuDrawState &s, const GpuDraw &d) { uint64_t t0 = PTN(); gpu_draw_impl(s, d); if (t0) gpu_prof_add(PROF_DRAW, plat_ticks_ns() - t0); }
static void gpu_draw_impl(const GpuDrawState &s, const GpuDraw &d) {
    if (!gpu_available() || !pfnCmdBeginRendering) return;
    static long dl_frame = -1;
    if ((long)g_frame != dl_frame) { dl_frame = (long)g_frame; g_dl_idx = 0; }
    const long draw_idx = g_dl_idx++;
    if (rt_frame()) {
        char b[256];
        snprintf(b, sizeof b, "draw %ld prim %d n %u inst %u prog %llx", draw_idx, d.prim, d.count, d.instances, (unsigned long long)s.program);
        g_dl = b;
        for (int i = 0; i < s.num_colors && i < 8; i++) if (s.color[i])
            dl_add(" C%d=%dx%d%s/L%d.m%d f%d cm%x%s", i, s.color[i]->w, s.color[i]->h, s.color[i]->depth3 ? "x3d" : "", s.color_layer[i], s.color_level[i], (int)s.color[i]->format,
                   s.channel_mask[i], (s.blend_enable >> i) & 1 ? " blend" : "");
        if (s.depth) dl_add(" D#%d", frame_rt_index(s.depth));
        if (s.depth) dl_add(" D=%dx%d/L%d%s%s func%d%s", s.depth->w, s.depth->h, s.depth_layer, s.depth_test ? " test" : "", s.depth_write ? " write" : "",
                            s.depth_func, s.stencil_test ? " STENCIL" : "");
        if (s.blend_enable & 1) dl_add(" blend0 %d,%d,%d/%d,%d,%d", s.blend[0].src_c, s.blend[0].dst_c, s.blend[0].eq_c, s.blend[0].src_a, s.blend[0].dst_a, s.blend[0].eq_a);
        dl_add(" cull%d front%d", s.cull_face, s.front_face);
        dl_add(" vp %d,%d,%d,%d sc %d,%d,%d,%d", s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3], s.scissor[0], s.scissor[1], s.scissor[2], s.scissor[3]);
        for (int st = 0; st < GS_COUNT; st++) if (s.shader[st]) dl_add(" sh%d", st);
    }
    if (!s.shader[GS_VERTEX]) { SKIP(0); return; }   // vertex-only programs (depth / shadow passes) are fine
    VkPrimitiveTopology topo;
    if (!topology(d.prim, topo)) { log_once("prim" + std::to_string(d.prim), "[gpu] unsupported primitive %d", d.prim); SKIP(1); return; }
    begin_frame();
    make_dummies();
    uint64_t pt0 = PTN();
    ProgObj *p = get_program(s);
    uint64_t pt1 = PTN(); g_dsub[0] += pt1 - pt0;
    if (!p->ok) { SKIP(2); return; }

    // 1. descriptors (may create/upload textures, so before the render pass starts). The arrays are reused (no
    // allocations per draw); their capacity is fixed before any pointer into them is taken.
    VkDescriptorSet set = VK_NULL_HANDLE;
    static std::vector<VkWriteDescriptorSet> writes; writes.clear(); writes.reserve(std::max<size_t>(64, p->desc.bindings.size()));
    static std::vector<VkDescriptorBufferInfo> bufs; bufs.clear(); bufs.reserve(256);
    static std::vector<VkDescriptorImageInfo> imgs; imgs.clear(); imgs.reserve(256);
    static std::vector<VkBufferView> texels; texels.clear(); texels.reserve(64);
    static std::vector<std::pair<uint32_t, uint32_t>> dyn; dyn.clear();   // (binding << 8 | element, dynamic offset)
    static const GpuSampler default_sampler;
    for (const ShaderBinding &b : p->desc.bindings) {
        VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set; w.dstBinding = b.binding; w.descriptorCount = b.count;
        size_t first;
        switch (b.kind) {
        case ShaderBinding::UNIFORM: {
            w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; first = bufs.size();
            for (uint32_t i = 0; i < b.count; i++) {
                uint32_t bank = b.bank + i;
                if (bank == 0) {   // driver bank 0: synthesize this stage's storage buffer table
                    const uint32_t size = 0x1200;
                    // the same table as the previous draw of this stage (this frame): reuse it
                    static struct { uint64_t frame = ~0ull; uint64_t addr[16], sz[16]; VkBuffer cb; VkDeviceSize co; } last[GS_COUNT];
                    auto &L = last[b.stage];
                    if (L.frame == g_frame && !memcmp(L.addr, s.ssbo_addr[b.stage], sizeof L.addr) && !memcmp(L.sz, s.ssbo_size[b.stage], sizeof L.sz)) {
                        bufs.push_back({L.cb, L.co, size});
                        continue;
                    }
                    VkBuffer cb; VkDeviceSize co; uint8_t *ptr;
                    if (ring_alloc(g_dynidx, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, size + 256, cb, co, ptr)) {
                        VkDeviceSize al = g_limits.minUniformBufferOffsetAlignment ? g_limits.minUniformBufferOffsetAlignment : 256;
                        VkDeviceSize pad = (al - co % al) % al; ptr += pad; co += pad;
                        memset(ptr, 0, size);
                        uint32_t base = ssbo_base(b.stage);   // = c0_word's layout: {addr lo, addr hi, size} every 16 bytes
                        for (int sl = 0; sl < 16; sl++) {
                            uint32_t o = base + 16 * sl;
                            if (o + 12 > size) break;
                            uint64_t a = s.ssbo_addr[b.stage][sl];
                            uint32_t w[3] = {(uint32_t)a, (uint32_t)(a >> 32), (uint32_t)s.ssbo_size[b.stage][sl]};
                            memcpy(ptr + o, w, 12);
                        }
                        bufs.push_back({cb, co, size});
                        L.frame = g_frame; memcpy(L.addr, s.ssbo_addr[b.stage], sizeof L.addr); memcpy(L.sz, s.ssbo_size[b.stage], sizeof L.sz); L.cb = cb; L.co = co;
                    } else bufs.push_back({g_dummy_buf, 0, 65536});
                    continue;
                }
                uint64_t a = bank < 18 ? s.ubo_addr[b.stage][bank] : 0, sz = bank < 18 ? s.ubo_size[b.stage][bank] : 0;
                VkBuffer buf; VkDeviceSize off;
                VkDeviceSize range = std::min<uint64_t>(sz ? sz : 16, std::min<uint64_t>(65536, g_limits.maxUniformBufferRange));
                if (a && lookup_buffer(a, range, buf, off) && off % g_limits.minUniformBufferOffsetAlignment == 0) bufs.push_back({buf, off, range});
                else { dl_add(" U%d.%u:MISS(%llx%s)", b.stage, bank, (unsigned long long)a, a ? (off % g_limits.minUniformBufferOffsetAlignment ? " align" : " lookup") : ""); bufs.push_back({g_dummy_buf, 0, 65536}); }
            }
            if (p->dyn_ubo) {
                w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                for (size_t k = first; k < bufs.size(); k++) { dyn.push_back({b.binding << 8 | (uint32_t)(k - first), (uint32_t)bufs[k].offset}); bufs[k].offset = 0; }
            }
            w.pBufferInfo = &bufs[first];
            break;
        }
        case ShaderBinding::STORAGE: {
            w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; first = bufs.size();
            for (uint32_t i = 0; i < b.count; i++) {
                uint64_t ub = b.cbuf_index < 18 ? s.ubo_addr[b.stage][b.cbuf_index] : 0;
                uint32_t o = b.cbuf_offset + i * 16;
                VkBuffer buf; VkDeviceSize off; bool ok = false;
                uint64_t a = 0; uint32_t sz = 0;
                if (b.cbuf_index == 0) { a = ((uint64_t)c0_word(s, b.stage, o + 4) << 32) | c0_word(s, b.stage, o); sz = c0_word(s, b.stage, o + 8); }
                else if (ub && o + 12 <= s.ubo_size[b.stage][b.cbuf_index]) { a = GV64(ub + o); sz = GV32(ub + o + 8); }
                {
                    uint64_t al = 64;   // = HostTranslateInfo::min_ssbo_alignment in gpu_shader.cpp (shader subtracts addr & ~63)
                    uint64_t a0 = a & ~(al - 1);
                    if (a && sz && lookup_buffer(a0, sz + (a - a0), buf, off)) { bufs.push_back({buf, off, sz + (a - a0)}); ok = true; }
                }
                dl_add(" S%d.%u=%llx+%u%s", b.stage, b.binding, (unsigned long long)a, sz, ok ? "" : ":MISS");
                if (!ok) {
                    log_once("ssbo" + std::to_string(b.cbuf_index) + "_" + std::to_string(b.cbuf_offset),
                             "[gpu] storage buffer unresolved: bank %u off %#x -> addr %llx size %u", b.cbuf_index, o, (unsigned long long)a, sz);
                    bufs.push_back({g_dummy_buf, 0, 65536});
                }
            }
            w.pBufferInfo = &bufs[first];
            break;
        }
        case ShaderBinding::TEXEL_BUFFER: case ShaderBinding::IMAGE_BUFFER:
            w.descriptorType = b.kind == ShaderBinding::TEXEL_BUFFER ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
            first = texels.size();
            for (uint32_t i = 0; i < b.count; i++) {
                uint32_t handle = 0;
                if (b.kind == ShaderBinding::TEXEL_BUFFER) {
                    if (b.cbuf_index == SHADER_BOUND_TEX_BANK) {
                        int slot = bound_tex_slot(b.cbuf_offset) + (int)i;
                        if (slot >= 0 && slot < 32) handle = (uint32_t)s.bound_tex[b.stage][slot];
                    } else {
                        g_q_state = &s;
                        uint32_t lo = q_read_cbuf(nullptr, b.stage, b.cbuf_index, b.cbuf_offset + i * 8) << b.shift_left;
                        uint32_t hi = b.has_secondary ? q_read_cbuf(nullptr, b.stage, b.sec_index, b.sec_offset) << b.sec_shift_left : 0;
                        handle = lo | hi;
                    }
                }
                VkBufferView v = handle ? texel_view(handle & 0xfffff) : VK_NULL_HANDLE;
                dl_add(" TB%d.%u=%x%s", b.stage, b.binding, handle & 0xfffff, v ? "" : ":MISSING");
                texels.push_back(v ? v : g_dummy_texel);
            }
            w.pTexelBufferView = &texels[first];
            break;
        case ShaderBinding::SAMPLED: {
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; first = imgs.size();
            for (uint32_t i = 0; i < b.count; i++) {
                uint32_t handle = 0;
                if (b.cbuf_index == SHADER_BOUND_TEX_BANK) {
                    int slot = bound_tex_slot(b.cbuf_offset) + (int)i;
                    if (slot >= 0 && slot < 32) handle = (uint32_t)s.bound_tex[b.stage][slot];
                } else {
                    g_q_state = &s;
                    uint32_t lo = q_read_cbuf(nullptr, b.stage, b.cbuf_index, b.cbuf_offset + i * 8) << b.shift_left;
                    uint32_t hi = b.has_secondary ? q_read_cbuf(nullptr, b.stage, b.sec_index, b.sec_offset) << b.sec_shift_left : 0;
                    handle = lo | hi;
                }
                // texture ID -> (GPU texture, flags, swizzle): one resolution per ID per frame (memo), not three
                struct TexRes { GpuTex *t; int flags; uint32_t swz; };
                // (direct-mapped, tagged with the ID and a stamp = frame + texture generation: no clearing, no allocation)
                struct TexMemo { uint64_t stamp; uint32_t tid; TexRes r; };
                static std::vector<TexMemo> memo(8192);
                uint64_t gen = nvn_tex_generation();
                const uint64_t stamp = g_frame * 0x100000001ull ^ gen;
                uint32_t tid = handle & 0xfffff;
                TexRes tr{nullptr, 0, 0x05040302};
                if (handle && g_res.texture) {
                    TexMemo &mm = memo[(tid * 2654435761u >> 19) & 8191];
                    if (mm.stamp == stamp && mm.tid == tid) tr = mm.r;
                    else {
                        tr.t = g_res.texture(g_res.user, tid);
                        if (tr.t && g_res.texture_flags) tr.flags = g_res.texture_flags(g_res.user, tid);
                        if (tr.t && g_res.texture_swizzle) tr.swz = g_res.texture_swizzle(g_res.user, tid);
                        if (nvn_tex_generation() == gen) { mm.stamp = stamp; mm.tid = tid; mm.r = tr; }   // (resolution itself may create textures)
                    }
                }
                GpuTex *t = tr.t;
                if (t) {
                    // A draw sampling an attachment of its own pass. On the Switch the game separates such draws with
                    // texture barriers (nvnCommandBufferBarrier) and each pixel reads what earlier draws wrote at its
                    // own location (G-buffer blending: ~130 draws a frame read 3-4 of the targets they write). As in
                    // Eden: make earlier writes visible (end the pass = full barrier, once per new write) and sample
                    // the image in place; every image stays in GENERAL layout. Reading without writing (decals sampling
                    // depth with depth writes off) is plain Vulkan. XRT_FEEDBACK_COPY=1: sample a copy whenever the draw
                    // also writes the image (the old behaviour: ~400 full-target copies a frame).
                    static int nofb = -1, always = -1, copy_mode = -1;
                    if (nofb < 0) { nofb = getenv("XRT_NO_FEEDBACK_COPY") ? 1 : 0; always = getenv("XRT_ALWAYS_FEEDBACK_COPY") ? 1 : 0; copy_mode = getenv("XRT_FEEDBACK_COPY") ? 1 : 0; }
                    bool written = false, attached = t == s.depth;
                    for (int c = 0; c < s.num_colors && c < 8; c++) if (s.color[c] == t) attached = true;
                    if (t == s.depth) {
                        bool stencil_writes = false;
                        if (s.stencil_test)
                            for (int f = 0; f < 2; f++)
                                if (s.stencil_wmask[f] && (s.stencil_op[f][0] != 1 || s.stencil_op[f][1] != 1 || s.stencil_op[f][2] != 1)) stencil_writes = true;
                        written = (s.depth_test && s.depth_write) || stencil_writes || always;
                    }
                    for (int c = 0; c < s.num_colors && c < 8; c++) if (s.color[c] == t && (s.channel_mask[c] || always)) written = true;
                    if (written && copy_mode && !nofb) { GpuTex *cp = feedback_copy(t); if (cp != t) { dl_add(" FBCOPY"); t = cp; } }
                    else if (attached && !nofb) {   // read in place: earlier attachment writes must be visible to texture reads
                        if (t->read_serial != t->write_serial) { gpu_end_pass(); t->read_serial = t->write_serial; dl_add(" FBSYNC"); }
                        dl_add(" FBREAD");
                    }
                }
                int tflags = t ? tr.flags : 0;
                uint32_t swz = t ? tr.swz : 0x05040302;
                VkImageView v = t ? ((tflags & 2) ? stencil_view(t) : tex_view_swz(t, b.tex_type, swz)) : VK_NULL_HANDLE;
                if (t) dl_add(" T%d.%u=%x:%dx%dx%d f%d tt%d rt%d", b.stage, b.binding, handle & 0xfffff, t->w, t->h, t->depth3 ? t->depth3 : t->layers, (int)t->format, b.tex_type, frame_rt_index(t));
                if (t && !g_dl.empty() && t->w * t->h <= 4 && g_res.texture_addr) {   // tiny textures: guest memory vs our GPU copy
                    uint64_t ga = g_res.texture_addr(g_res.user, handle & 0xfffff);
                    if (ga) { const uint8_t *m = (const uint8_t *)(uintptr_t)ga; dl_add("[mem %llx: %02x%02x%02x%02x%02x%02x%02x%02x]", (unsigned long long)ga, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7]); }
                    static std::set<GpuTex *> shown; static uint64_t shown_frame;
                    if (shown_frame != g_frame) { shown.clear(); shown_frame = g_frame; }
                    if (shown.insert(t).second && !t->depth_kind && readback_rgba8_raw(t)) {
                        const uint8_t *g = (const uint8_t *)g_rb_map; dl_add("[gpu %02x%02x%02x%02x%02x%02x%02x%02x]", g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7]);
                    }
                }
                if (!t) dl_add(" T%d.%u=%x:MISSING tt%d", b.stage, b.binding, handle, b.tex_type);
                if (!v) v = dummy_view(b.tex_type);
                // sampler ID -> VkSampler, resolved once per ID until the sampler pools change
                static struct { uint64_t gen = ~0ull; VkSampler s; } smemo[4096];
                VkSampler vs;
                if (!handle || !g_res.sampler) { static VkSampler def = get_sampler(default_sampler); vs = def; }
                else {
                    auto &m = smemo[(handle >> 20) & 0xfff];
                    if (m.gen != gen) { GpuSampler sm; bool have = g_res.sampler(g_res.user, (handle >> 20) & 0xfff, &sm); m.s = get_sampler(have ? sm : default_sampler); m.gen = gen; }
                    vs = m.s;
                }
                imgs.push_back({vs, v, VK_IMAGE_LAYOUT_GENERAL});
            }
            w.pImageInfo = &imgs[first];
            break;
        }
        case ShaderBinding::IMAGE:
            w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; first = imgs.size();
            for (uint32_t i = 0; i < b.count; i++) {
                VkImageView v = VK_NULL_HANDLE;
                if (b.cbuf_index == SHADER_BOUND_TEX_BANK && b.cbuf_offset >= SHADER_BOUND_IMG_OFF && g_res.image) {
                    int slot = (int)(b.cbuf_offset - SHADER_BOUND_IMG_OFF) / 8 + (int)i, level = 0;
                    uint32_t h = slot < 8 ? (uint32_t)s.bound_img[b.stage][slot] : 0;
                    GpuTex *t = h ? g_res.image(g_res.user, h & 0xfffff, &level) : nullptr;
                    if (t) v = storage_view(t, level, b.tex_type);
                    dl_add(" I%d.%u=%x:%s", b.stage, b.binding, h, t ? (v ? "ok" : "noview") : "MISSING");
                }
                if (!v) v = tex_view(g_dummy_storage, TT_2D);
                imgs.push_back({VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL});
            }
            w.pImageInfo = &imgs[first];
            break;
        }
        writes.push_back(w);
    }
    if (!writes.empty()) {   // reuse a set written earlier this frame with the same contents, or write a new one
        static std::vector<uint64_t> key; key.clear();
        key.push_back((uint64_t)(uintptr_t)p->dsl);
        for (const VkWriteDescriptorSet &w : writes) {
            key.push_back((uint64_t)w.dstBinding << 40 | (uint64_t)w.descriptorType << 32 | w.descriptorCount);
            for (uint32_t i = 0; i < w.descriptorCount; i++) {
                if (w.pBufferInfo) { key.push_back((uint64_t)(uintptr_t)w.pBufferInfo[i].buffer); key.push_back(w.pBufferInfo[i].offset); key.push_back(w.pBufferInfo[i].range); }
                else if (w.pImageInfo) { key.push_back((uint64_t)(uintptr_t)w.pImageInfo[i].imageView); key.push_back((uint64_t)(uintptr_t)w.pImageInfo[i].sampler); }
                else if (w.pTexelBufferView) key.push_back((uint64_t)(uintptr_t)w.pTexelBufferView[i]);
            }
        }
        uint64_t h = 1469598103934665603ULL;
        for (uint64_t v : key) { h ^= v; h *= 1099511628211ULL; h ^= h >> 31; }
        SetSlot *slot = nullptr;
        if (!desc_cache_off()) {
            if (!h) h = 1;
            for (size_t i = 0; i < 16; i++) {   // (a full neighbourhood: the set is written anew, not cached)
                SetSlot &sl = g_set_cache[(h + i) & (g_set_cache.size() - 1)];
                if (sl.epoch != g_set_epoch) { if (!slot) slot = &sl; break; }
                if (sl.hash == h) { slot = &sl; break; }
            }
        }
        if (slot && slot->epoch == g_set_epoch && slot->hash == h) { set = slot->set; writes.clear(); g_set_hits++; }
        else {
            set = alloc_set(p->dsl);
            if (!set) { SKIP(3); return; }
            for (VkWriteDescriptorSet &w : writes) w.dstSet = set;
            if (slot) { slot->hash = h; slot->epoch = g_set_epoch; slot->set = set; }
            g_set_misses++;
        }
        std::sort(dyn.begin(), dyn.end());   // dynamic offsets go in binding order
    }
    uint64_t pt2 = PTN(); g_dsub[1] += pt2 - pt1;
    // vertex + index buffers
    VkBuffer vbuf[16]; VkDeviceSize voff[16]; bool used_stream[16] = {};
    for (int i = 0; i < 16; i++) if ((p->desc.vertex_inputs & (1u << i)) && i < s.num_attribs) used_stream[s.attribs[i].stream & 15] = true;
    for (int st = 0; st < 16; st++) {
        if (!used_stream[st]) continue;
        if (!s.vb_addr[st] || !lookup_buffer(s.vb_addr[st], 1, vbuf[st], voff[st])) { SKIP(4); log_once("vb", "[gpu] vertex buffer not in any memory pool (draw skipped)"); return; }
    }
    VkBuffer ibuf = VK_NULL_HANDLE; VkDeviceSize ioff = 0; VkIndexType itype = VK_INDEX_TYPE_UINT16;
    uint32_t draw_count = d.count; bool draw_indexed = d.indexed;
    if (d.prim == 7 && !d.xfb_count_addr) {
        // quads -> two triangles each, through a generated 32-bit index list
        uint32_t quads = d.count / 4; uint8_t *ptr;
        if (!quads || !ring_alloc(g_dynidx, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, (VkDeviceSize)quads * 24, ibuf, ioff, ptr)) { SKIP(5); return; }
        uint32_t *out = (uint32_t *)ptr;
        for (uint32_t q = 0; q < quads; q++) {
            uint32_t v[4];
            for (int k = 0; k < 4; k++) {
                uint32_t i = q * 4 + k;
                if (!d.indexed) v[k] = d.first + i;
                else if (d.index_type == 2) v[k] = GV32(d.index_addr + 4ULL * i);
                else if (d.index_type == 1) { uint16_t x; memcpy(&x, gview(d.index_addr + 2ULL * i), 2); v[k] = x; }
                else v[k] = *gview(d.index_addr + i);
            }
            out[6 * q + 0] = v[0]; out[6 * q + 1] = v[1]; out[6 * q + 2] = v[2];
            out[6 * q + 3] = v[0]; out[6 * q + 4] = v[2]; out[6 * q + 5] = v[3];
        }
        itype = VK_INDEX_TYPE_UINT32; draw_count = quads * 6; draw_indexed = true;
    } else if (d.indexed) {
        if (d.index_type == 0) {   // 8-bit indices: widen to 32-bit
            uint8_t *ptr;
            if (!d.count || !ring_alloc(g_dynidx, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, (VkDeviceSize)d.count * 4, ibuf, ioff, ptr)) { SKIP(5); return; }
            for (uint32_t i = 0; i < d.count; i++) ((uint32_t *)ptr)[i] = *gview(d.index_addr + i);
            itype = VK_INDEX_TYPE_UINT32;
        } else {
            if (d.index_type == 2) itype = VK_INDEX_TYPE_UINT32;
            if (!lookup_buffer(d.index_addr, 1, ibuf, ioff)) { SKIP(5); log_once("ib", "[gpu] index buffer not in any memory pool (draw skipped)"); return; }
        }
    }
    {   // XRT_DEBUG_DRAW=frame,n: dump the n-th draw of that frame (state, buffers, SPIR-V) to debugdraw/
        // XRT_DEBUG_DRAW=frame,n | frame,a-b (range, one folder per draw) | frame,-1 (40 draws from the first 3D target)
        static long dfr = -2, dn, dn_end = -1; static long auto_start = -1, auto_count = 0;
        if (dfr == -2) { dfr = -1; if (const char *e = getenv("XRT_DEBUG_DRAW")) { int k = sscanf(e, "%ld,%ld-%ld", &dfr, &dn, &dn_end); if (k < 3) dn_end = -1; } }
        bool hit = false;
        if (dfr >= 0 && (long)g_frame == dfr) {
            if (dn_end >= dn && dn >= 0) hit = draw_idx >= dn && draw_idx <= dn_end;
            else if (dn >= 0) hit = draw_idx == dn;
            else {
                if (auto_start < 0 && s.color[0] && s.color[0]->depth3) auto_start = draw_idx;
                hit = auto_start >= 0 && auto_count < 40 && ++auto_count;
            }
        }
        // F12 capture: also dump every blended draw into a half-float target (effects buffers), to trace NaNs
        bool cap_hit = !hit && rt_frame() && s.num_colors > 0 && s.color[0] && s.color[0]->format == VK_FORMAT_R16G16B16A16_SFLOAT &&
                       (s.blend_enable & 1) && !getenv("XRT_NO_FX_DUMP");
        hit = hit || cap_hit;
        char dir[64] = "debugdraw";
        if (hit) {
#ifdef _WIN32
            _mkdir("debugdraw");
#else
            mkdir("debugdraw", 0755);
#endif
            if (cap_hit) snprintf(dir, sizeof dir, "debugdraw/%05llu_%04ld", (unsigned long long)g_frame + 1, draw_idx);
            else if (dn < 0 || dn_end >= 0) snprintf(dir, sizeof dir, "debugdraw/%04ld", draw_idx);
        }
        auto P = [&](const char *n) { static char b[128]; snprintf(b, sizeof b, "%s/%s", dir, n); return b; };
        if (hit) {
            {
#ifdef _WIN32
                _mkdir(dir);
#else
                mkdir(dir, 0755);
#endif
                auto wr = [](const char *n, const void *p, size_t sz) { if (FILE *f = fopen(n, "wb")) { fwrite(p, 1, sz, f); fclose(f); } };
                FILE *f = fopen(P("state.txt"), "w");
                fprintf(f, "prim %d indexed %d type %d count %u first %u inst %u base_vertex %d\n", d.prim, d.indexed, d.index_type, d.count, d.first, d.instances, d.base_vertex);
                fprintf(f, "viewport %d %d %d %d depth %g %g scissor %d %d %d %d\n", s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3], s.depth_range[0], s.depth_range[1], s.scissor[0], s.scissor[1], s.scissor[2], s.scissor[3]);
                fprintf(f, "colors %d: %dx%d fmt %d; vertex inputs %x frag outputs %x\n", s.num_colors, s.color[0] ? s.color[0]->w : 0, s.color[0] ? s.color[0]->h : 0, s.color[0] ? (int)s.color[0]->format : 0, p->desc.vertex_inputs, p->desc.frag_outputs);
                for (int i = 0; i < s.num_attribs; i++) fprintf(f, "attr %d: fmt %d off %u stream %d\n", i, s.attribs[i].format, s.attribs[i].offset, s.attribs[i].stream);
                for (int i = 0; i < 4; i++) fprintf(f, "stream %d: stride %u div %u vb %llx size %llu\n", i, s.streams[i].stride, s.streams[i].divisor, (unsigned long long)s.vb_addr[i], (unsigned long long)s.vb_size[i]);
                for (int st = 0; st < GS_COUNT; st++) for (int b = 0; b < 18; b++) if (s.ubo_addr[st][b]) {
                    fprintf(f, "ubo stage %d bank %d: %llx size %llu\n", st, b, (unsigned long long)s.ubo_addr[st][b], (unsigned long long)s.ubo_size[st][b]);
                    char n[128]; snprintf(n, sizeof n, "%s/ubo_%d_%d.bin", dir, st, b); wr(n, (void *)(uintptr_t)s.ubo_addr[st][b], (size_t)std::min<uint64_t>(s.ubo_size[st][b], 65536));
                }
                for (const ShaderBinding &b : p->desc.bindings) fprintf(f, "binding %u kind %d stage %d count %u bank %u cbuf %u+%#x type %d\n", b.binding, (int)b.kind, b.stage, b.count, b.bank, b.cbuf_index, b.cbuf_offset, b.tex_type);
                for (int st = 0; st < GS_COUNT; st++) if (!p->desc.spirv[st].empty()) { char n[128]; snprintf(n, sizeof n, "%s/stage%d.spv", dir, st); wr(n, p->desc.spirv[st].data(), p->desc.spirv[st].size() * 4); }
                for (int st = 0; st < GS_COUNT; st++) if (s.shader[st]) { char n[128]; snprintf(n, sizeof n, "%s/stage%d.code", dir, st); wr(n, s.shader[st], s.shader_end[st] - s.shader[st]); }
                if (s.vb_addr[0]) wr(P("vb0.bin"), (void *)(uintptr_t)s.vb_addr[0], (size_t)std::min<uint64_t>(s.vb_size[0], 1 << 20));
                if (d.indexed) wr(P("ib.bin"), (void *)(uintptr_t)d.index_addr, d.count * (d.index_type == 2 ? 4 : 2));
                fclose(f);
                if (!cap_hit) LOG(0, "[gpu] dumped draw %ld of frame %ld to %s/", draw_idx, dfr, dir);
            }
        }
    }
    uint64_t pt3 = PTN(); g_dsub[2] += pt3 - pt2;
    VkPipeline pipe = get_pipeline(s, p, topo);
    uint64_t pt4 = PTN(); g_dsub[3] += pt4 - pt3;
    if (!pipe) { SKIP(6); return; }
    if (!writes.empty()) vkUpdateDescriptorSets(g_dev, (uint32_t)writes.size(), writes.data(), 0, nullptr);
    g_dsub[4] += PTN() - pt4;

    const bool dd = dd_wanted(s);
    if (dd) dd_before(s);
    // 2. render pass + draw
    if (!begin_pass(s)) { SKIP(7); return; }
    // Command state already set in this recording is not set again (~15 calls into the driver per draw otherwise).
    // Dynamic state survives pipeline changes (every pipeline has the same dynamic states, depth bias aside).
    static struct BoundState {
        uint64_t gen = ~0ull; VkPipeline pipe; VkPipelineLayout pc_layout; VkViewport vp; VkRect2D sc; float blend[4]; float bias[3]; bool bias_ok;
        uint32_t stencil[6]; bool stencil_set; VkBuffer vb[16]; VkDeviceSize vo[16]; VkBuffer ib; VkDeviceSize io; VkIndexType it;
    } B;
    static int no_filter = -1; if (no_filter < 0) no_filter = getenv("XRT_NO_STATE_FILTER") ? 1 : 0;
    if (B.gen != g_rec_gen || no_filter) { B = BoundState{}; B.gen = g_rec_gen; }
    const bool want_bias = depth_bias_on(s);
    if (B.pipe != pipe) { vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); B.pipe = pipe; if (!want_bias) B.bias_ok = false; }
    if (set) {
        uint32_t offs[64]; uint32_t no = 0;
        for (auto &dv : dyn) if (no < 64) offs[no++] = dv.second;
        vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, 1, &set, no, offs);
    }
    if (p->pc_stages && B.pc_layout != p->layout) {
        uint32_t pc[16] = {}; pc[8] = 0x3f800000;   // RescalingLayout.down_factor = 1.0
        vkCmdPushConstants(g_cmd, p->layout, p->pc_stages, 0, 64, pc);
        B.pc_layout = p->layout;
    }
    // NVN window origin mode 1 (UPPER_LEFT): rect (x, y, w, h) covers rows [y, y + h) from the top and NDC +Y
    // maps to row y. Verified on the battle HUD: per-element viewports like (90, 366, 144, 24) for HP bars.
    const int H = (int)g_pass.ext.height;
    VkViewport vp = {(float)s.viewport[0], (float)s.viewport[1], (float)s.viewport[2], (float)s.viewport[3], s.depth_range[0], s.depth_range[1]};
    if (vp.width <= 0 || vp.height <= 0) { vp.x = 0; vp.y = 0; vp.width = (float)g_pass.ext.width; vp.height = (float)H; }
    // XRT_VP_MODE=1: old bottom-anchored mapping (H - y), for comparison
    static int vp_mode = getenv("XRT_VP_MODE") ? atoi(getenv("XRT_VP_MODE")) : 0;
    if (s.lower_left) { vp.y = vp_mode == 1 ? (float)H - vp.y : vp.y + vp.height; vp.height = -vp.height; }
    if (memcmp(&B.vp, &vp, sizeof vp)) { vkCmdSetViewport(g_cmd, 0, 1, &vp); B.vp = vp; }
    int sx = s.scissor[0], sy = s.scissor[1], sw = s.scissor[2], sh = s.scissor[3];
    if (sw > 0x10000) sw = (int)g_pass.ext.width;
    if (sh > 0x10000) sh = H;
    if (s.lower_left && vp_mode == 1) sy = H - sy - sh;
    if (sx < 0) { sw += sx; sx = 0; }
    if (sy < 0) { sh += sy; sy = 0; }
    sw = std::min(sw, (int)g_pass.ext.width - sx); sh = std::min(sh, H - sy);
    VkRect2D sc = {{sx, sy}, {(uint32_t)std::max(0, sw), (uint32_t)std::max(0, sh)}};
    if (memcmp(&B.sc, &sc, sizeof sc)) { vkCmdSetScissor(g_cmd, 0, 1, &sc); B.sc = sc; }
    if (memcmp(B.blend, s.blend_color, sizeof B.blend)) { vkCmdSetBlendConstants(g_cmd, s.blend_color); memcpy(B.blend, s.blend_color, sizeof B.blend); }
    if (want_bias) {
        float bias[3] = {s.poly_offset[1], s.poly_offset[2], s.poly_offset[0]};   // (units, clamp, slope factor)
        if (!B.bias_ok || memcmp(B.bias, bias, sizeof bias)) { vkCmdSetDepthBias(g_cmd, bias[0], bias[1], bias[2]); memcpy(B.bias, bias, sizeof bias); B.bias_ok = true; }
    }
    {   // stencil dynamic state (front/back swapped like the pipeline when the viewport is flipped)
        int fi = 0, bi = 1;
        uint32_t st6[6] = {(uint32_t)s.stencil_ref[fi], (uint32_t)s.stencil_ref[bi], (uint32_t)s.stencil_vmask[fi], (uint32_t)s.stencil_vmask[bi], (uint32_t)s.stencil_wmask[fi], (uint32_t)s.stencil_wmask[bi]};
        if (memcmp(B.stencil, st6, sizeof st6) || !B.stencil_set) {
            vkCmdSetStencilReference(g_cmd, VK_STENCIL_FACE_FRONT_BIT, st6[0]); vkCmdSetStencilReference(g_cmd, VK_STENCIL_FACE_BACK_BIT, st6[1]);
            vkCmdSetStencilCompareMask(g_cmd, VK_STENCIL_FACE_FRONT_BIT, st6[2]); vkCmdSetStencilCompareMask(g_cmd, VK_STENCIL_FACE_BACK_BIT, st6[3]);
            vkCmdSetStencilWriteMask(g_cmd, VK_STENCIL_FACE_FRONT_BIT, st6[4]); vkCmdSetStencilWriteMask(g_cmd, VK_STENCIL_FACE_BACK_BIT, st6[5]);
            memcpy(B.stencil, st6, sizeof st6); B.stencil_set = true;
        }
    }
    for (int st = 0; st < 16; st++)
        if (used_stream[st] && (B.vb[st] != vbuf[st] || B.vo[st] != voff[st])) { vkCmdBindVertexBuffers(g_cmd, (uint32_t)st, 1, &vbuf[st], &voff[st]); B.vb[st] = vbuf[st]; B.vo[st] = voff[st]; }
    bool xfb_on = false, xfb_ctr_ok = false; VkBuffer xfb_cb = VK_NULL_HANDLE; VkDeviceSize xfb_co = 0;
    if (s.xfb_active && s.xfb_count && s.xfb_addr && s.xfb_size && pfnCmdBindXfb && pfnCmdBeginXfb && pfnCmdEndXfb) {
        VkBuffer xb; VkDeviceSize xo;
        if (lookup_buffer(s.xfb_addr, s.xfb_size, xb, xo) && xo % 4 == 0) {
            VkDeviceSize xs = s.xfb_size;
            pfnCmdBindXfb(g_cmd, 0, 1, &xb, &xo, &xs);
            // NVN keeps writing where the previous draw of the same Begin stopped: the byte counter lives in the
            // control buffer at +4 (where Eden reads it for DrawTransformFeedback). The first draw after Begin starts at
            // 0; later ones resume from the counter.
            xfb_ctr_ok = s.xfb_control && lookup_buffer(s.xfb_control + 4, 4, xfb_cb, xfb_co) && xfb_co % 4 == 0;
            if (xfb_ctr_ok && !s.xfb_fresh) pfnCmdBeginXfb(g_cmd, 0, 1, &xfb_cb, &xfb_co);
            else pfnCmdBeginXfb(g_cmd, 0, 0, nullptr, nullptr);
            xfb_on = true; dl_add(" XFB%s", s.xfb_fresh ? "" : "+");
        } else log_once("xfbbuf", "[gpu] transform feedback buffer %llx not usable", (unsigned long long)s.xfb_addr);
    }
    if (d.xfb_count_addr) {
        VkBuffer cb; VkDeviceSize co;
        uint32_t stride = s.streams[0].stride;
        if (pfnCmdDrawByteCount && stride && lookup_buffer(d.xfb_count_addr + 4, 4, cb, co)) {   // byte count at control + 4
            pfnCmdDrawByteCount(g_cmd, d.instances, 0, cb, co, 0, stride);
            dl_add(" DRAW-XFB stride %u", stride);
        } else log_once("xfbdraw", "[gpu] DrawTransformFeedback: counter %llx or stride %u unusable", (unsigned long long)d.xfb_count_addr, stride);
    } else if (draw_indexed) {
        if (B.ib != ibuf || B.io != ioff || B.it != itype) { vkCmdBindIndexBuffer(g_cmd, ibuf, ioff, itype); B.ib = ibuf; B.io = ioff; B.it = itype; }
        vkCmdDrawIndexed(g_cmd, draw_count, d.instances, 0, d.indexed ? d.base_vertex : 0, d.base_instance);
    } else {
        vkCmdDraw(g_cmd, d.count, d.instances, d.first, d.base_instance);
    }
    if (xfb_on) { if (xfb_ctr_ok) pfnCmdEndXfb(g_cmd, 0, 1, &xfb_cb, &xfb_co); else pfnCmdEndXfb(g_cmd, 0, 0, nullptr, nullptr); }
    g_draw_ok++;
    if (g_pass.prof && !g_pp_frame.empty()) g_pp_frame.back().draws++;
    for (int c = 0; c < s.num_colors && c < 8; c++) if (s.color[c] && s.channel_mask[c]) s.color[c]->write_serial++;
    if (s.depth && (s.depth_write || s.stencil_test)) s.depth->write_serial++;
    if (s.rast_discard) { dl_add(" DISCARD"); gpu_end_pass(); }
    else if (p->desc.writes_memory) {
        // Maxwell runs draws in order; Vulkan overlaps them inside a render pass. A draw that stores to memory
        // (storage buffer / image) ends the pass, whose end is a full barrier, so later draws see its writes
        // (Monado T133/T136: skinning palettes and scratch buffers reused per mesh). XRT_NO_MEMW_BARRIER=1: off.
        static int off = -1; if (off < 0) off = getenv("XRT_NO_MEMW_BARRIER") ? 1 : 0;
        if (!off) { dl_add(" MEMW"); gpu_end_pass(); g_memw_barriers++; }
    }
    if (dd) dd_after(s, draw_idx);   // its storage-buffer writes are read by later draws: barrier
    count_rt_draw();
    {   static int att = -1; if (att < 0) { const char *e = getenv("XRT_SNAP_ATT"); att = e ? atoi(e) : 0; if (att < 0 || att > 7) att = 0; }
        snap_after_draw((long)g_frame, draw_idx, att < s.num_colors ? s.color[att] : nullptr); }
    if (!g_dl.empty()) {
        char r[64]; snprintf(r, sizeof r, "ok fragout %x rt", p->desc.frag_outputs);
        for (int i = 0; i < s.num_colors && i < 8; i++) if (s.color[i]) { char q[16]; snprintf(q, sizeof q, " %d", frame_rt_index(s.color[i])); strncat(r, q, sizeof r - strlen(r) - 1); }
        dl_flush(r);
    }

}

// Frames much longer than the recent ones, with what this frame had to build (XRT_HITCH_LOG=0: off). gpu_present.
static void hitch_report(uint64_t frame) {
    static int on = -1; if (on < 0) { const char *e = getenv("XRT_HITCH_LOG"); on = e ? atoi(e) : 1; }
    static uint64_t last; static double avg = 0;
    uint64_t now = plat_ticks_ns();
    if (last) {
        double ms = (now - last) / 1e6;
        if (on && avg > 0 && ms > 25 && ms > 2.5 * avg) {
            // where the time went: the render worker recording (incl. shaders / pipelines / textures), waiting for the GPU
            // or the swapchain, and the rest = waiting for the game's own threads (file loads and CPU work show there)
            double rec = g_prof_frame[PROF_EXEC] / 1e6, gw = (g_prof_frame[PROF_WAIT] + g_prof_frame[PROF_ACQUIRE]) / 1e6;
            LOG(1, "[hitch] frame %llu: %.1f ms (usual %.1f) - shaders %llu new + %llu from disk %.1f ms, pipelines %llu %.1f ms, textures %llu new %.1f ms, "
                   "recording %.1f ms, GPU/swapchain wait %.1f ms, game threads ~%.1f ms; file reads %llu (%.1f MB, %.1f ms), missing files %llu",
                (unsigned long long)frame, ms, avg, (unsigned long long)g_hitch_prog_new, (unsigned long long)g_hitch_prog_disk, g_hitch_prog_ns / 1e6,
                (unsigned long long)g_hitch_pipe_n, g_hitch_pipe_ns / 1e6, (unsigned long long)g_tex_new.load(), g_tex_new_ns.load() / 1e6,
                rec, gw, std::max(0.0, ms - rec - gw), (unsigned long long)g_io_reads.load(), g_io_bytes.load() / 1048576.0, g_io_ns.load() / 1e6,
                (unsigned long long)g_io_missing.load());
        }
        avg = avg ? avg * 0.95 + std::min(ms, avg * 3) * 0.05 : ms;
    }
    last = now;
    g_hitch_prog_new = g_hitch_prog_disk = g_hitch_prog_ns = g_hitch_pipe_n = g_hitch_pipe_ns = 0;
    for (auto &p : g_prof_frame) p = 0;
    g_io_ns = g_io_bytes = g_io_reads = g_io_missing = g_tex_new = g_tex_new_ns = 0;
    if (frame % 1800 == 0 && (g_scache_hits || g_scache_misses))
        LOG(1, "[gpu] shader cache: %llu programs from disk (%llu with other texture handles), %llu translated this session", (unsigned long long)g_scache_hits, (unsigned long long)g_scache_hits_subst, (unsigned long long)g_scache_misses);
    if (frame % 1800 == 0 && g_pj_built.load())
        LOG(1, "[gpu] background pipelines: %llu built (%.0f ms in all), draws skipped while building %llu, waits %llu",
            (unsigned long long)g_pj_built.load(), g_pj_built_ns.load() / 1e6, (unsigned long long)g_pj_skipped, (unsigned long long)g_pj_waited);
}
