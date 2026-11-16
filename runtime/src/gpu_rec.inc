// Command recording thread (milestone M5). The render worker prepares every draw (state, resources, descriptors,
// pipelines) and a second thread makes the Vulkan command calls, the NVIDIA driver's share of the work: the two run in
// parallel. The vkCmd* / command buffer / queue entry points the backend uses are replaced here by stubs that copy their
// arguments into a ring buffer; the recorder thread replays them, in order, into the real driver functions. Calls that
// wait for or free something the recorded commands may use (fence waits, query reads, destroys, pool allocation) first
// let the recorder catch up (rec_drain). Everything else (object creation, descriptor writes) stays on the worker.
// Included by gpu_vk.cpp after the function pointer declarations. XRT_RECORD_THREAD=0: calls go to the driver directly.
#include <condition_variable>
#include <mutex>
#include <new>
#include <thread>
#include <tuple>

static const size_t RB_SIZE = 64u << 20;
static uint8_t *g_rb;
static std::atomic<size_t> g_rhead{0}, g_rtail{0};          // bytes published / bytes consumed (monotonic)
static std::atomic<bool> g_rsleep{false};
static std::mutex g_rm; static std::condition_variable g_rcv;
static bool g_rec_on;
static std::mutex g_qm;              // real queue / swapchain calls (recorder, frame pacer, render worker)
static void pacer_flush();
struct RecHdr { void (*run)(uint8_t *); uint32_t size, pad; };
static inline size_t RA(size_t n) { return (n + 15) & ~(size_t)15; }
static size_t g_rpend, g_rpend_size;                          // record being written (producer)

static void rec_wait_space(size_t upto) {   // producer: until bytes [.., upto) are free
    for (int i = 0; upto - g_rtail.load(std::memory_order_acquire) > RB_SIZE; i++) { if (i > 64) std::this_thread::yield(); }
}
static uint8_t *rec_begin(size_t payload) {
    size_t sz = RA(sizeof(RecHdr) + payload);
    size_t h = g_rhead.load(std::memory_order_relaxed);
    size_t off = h % RB_SIZE;
    if (off + sz > RB_SIZE) {   // no room before the end: a filler record up to the end, then from the start
        rec_wait_space(h + (RB_SIZE - off));
        RecHdr *f = (RecHdr *)(g_rb + off); f->run = nullptr; f->size = (uint32_t)(RB_SIZE - off);
        h += RB_SIZE - off; off = 0;
        g_rhead.store(h, std::memory_order_release);
    }
    rec_wait_space(h + sz);
    g_rpend = h; g_rpend_size = sz;
    return g_rb + off + sizeof(RecHdr);
}
static void rec_end(void (*run)(uint8_t *)) {
    RecHdr *hd = (RecHdr *)(g_rb + g_rpend % RB_SIZE);
    hd->run = run; hd->size = (uint32_t)g_rpend_size;
    g_rhead.store(g_rpend + g_rpend_size, std::memory_order_release);
    if (g_rsleep.load(std::memory_order_acquire)) { std::lock_guard<std::mutex> l(g_rm); g_rcv.notify_one(); }
}
static void rec_thread() {
    xrt_sampler_register_thread("command recorder (Vulkan)");
    for (;;) {
        size_t t = g_rtail.load(std::memory_order_relaxed);
        size_t h = g_rhead.load(std::memory_order_acquire);
        if (t == h) {
            bool got = false;
            for (int i = 0; i < 4000 && !got; i++) { if (g_rhead.load(std::memory_order_acquire) != t) got = true; else if (i > 200) std::this_thread::yield(); }
            if (!got) {
                std::unique_lock<std::mutex> l(g_rm);
                g_rsleep.store(true, std::memory_order_seq_cst);
                g_rcv.wait(l, [t] { return g_rhead.load(std::memory_order_acquire) != t; });
                g_rsleep.store(false, std::memory_order_relaxed);
            }
            continue;
        }
        RecHdr *hd = (RecHdr *)(g_rb + t % RB_SIZE);
        if (hd->run) hd->run((uint8_t *)(hd + 1));
        g_rtail.store(t + hd->size, std::memory_order_release);
    }
}
// the recorder has made every call queued so far
static void rec_drain() {
    if (!g_rec_on) return;
    size_t h = g_rhead.load(std::memory_order_acquire);
    for (int i = 0; g_rtail.load(std::memory_order_acquire) < h; i++) { if (i > 64) std::this_thread::yield(); }
}

// value-only calls: the arguments as they are
template <class Fn, class... A> static void rec_vals(Fn real, A... a) {
    using T = std::tuple<Fn, A...>;
    new (rec_begin(sizeof(T))) T(real, a...);
    rec_end([](uint8_t *q) { T *t = (T *)q; std::apply([](Fn f, A... x) { f(x...); }, *t); });
}
struct Pack {   // arrays copied after a stub's fixed part, in the same record
    uint8_t *p;
    template <class T> const T *arr(const T *src, uint32_t n) { if (!src || !n) return nullptr; T *d = (T *)p; memcpy((void *)d, src, n * sizeof(T)); p += RA(n * sizeof(T)); return d; }
};
template <class T> static size_t AS(const T *src, uint32_t n) { return src && n ? RA(n * sizeof(T)) : 0; }

#define RD(f) static PFN_##f r_##f;
RD(vkCmdBindPipeline) RD(vkCmdBindIndexBuffer) RD(vkCmdDraw) RD(vkCmdDrawIndexed) RD(vkCmdSetDepthBias)
RD(vkCmdSetStencilReference) RD(vkCmdSetStencilCompareMask) RD(vkCmdSetStencilWriteMask) RD(vkCmdFillBuffer)
RD(vkCmdWriteTimestamp) RD(vkCmdResetQueryPool) RD(vkEndCommandBuffer) RD(vkResetCommandBuffer)
RD(vkCmdBindDescriptorSets) RD(vkCmdPushConstants) RD(vkCmdSetViewport) RD(vkCmdSetScissor) RD(vkCmdSetBlendConstants)
RD(vkCmdBindVertexBuffers) RD(vkCmdCopyBuffer) RD(vkCmdCopyImage) RD(vkCmdCopyBufferToImage) RD(vkCmdCopyImageToBuffer)
RD(vkCmdBlitImage) RD(vkCmdClearColorImage) RD(vkCmdClearDepthStencilImage) RD(vkCmdPipelineBarrier)
RD(vkBeginCommandBuffer) RD(vkQueueSubmit) RD(vkQueuePresentKHR)
RD(vkWaitForFences) RD(vkResetFences) RD(vkGetQueryPoolResults) RD(vkDeviceWaitIdle) RD(vkAllocateCommandBuffers)
RD(vkAcquireNextImageKHR) RD(vkDestroyImage) RD(vkDestroyImageView) RD(vkDestroyBuffer) RD(vkFreeMemory) RD(vkDestroySwapchainKHR)
static PFN_vkCmdBeginRendering r_BeginRendering; static PFN_vkCmdEndRendering r_EndRendering;
static PFN_vkCmdDrawIndirectByteCountEXT r_DrawByteCount; static PFN_vkCmdBindTransformFeedbackBuffersEXT r_BindXfb;
static PFN_vkCmdBeginTransformFeedbackEXT r_BeginXfb; static PFN_vkCmdEndTransformFeedbackEXT r_EndXfb;

// value-only stubs
#define UNPAREN(...) __VA_ARGS__
#define S0(f, PARAMS, ARGS) static VKAPI_ATTR void VKAPI_CALL s_##f PARAMS { rec_vals(r_##f, UNPAREN ARGS); }
S0(vkCmdBindPipeline, (VkCommandBuffer c, VkPipelineBindPoint b, VkPipeline p), (c, b, p))
S0(vkCmdBindIndexBuffer, (VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkIndexType t), (c, b, o, t))
S0(vkCmdDraw, (VkCommandBuffer c, uint32_t a, uint32_t b, uint32_t d, uint32_t e), (c, a, b, d, e))
S0(vkCmdDrawIndexed, (VkCommandBuffer c, uint32_t a, uint32_t b, uint32_t d, int32_t e, uint32_t f), (c, a, b, d, e, f))
S0(vkCmdSetDepthBias, (VkCommandBuffer c, float a, float b, float d), (c, a, b, d))
S0(vkCmdSetStencilReference, (VkCommandBuffer c, VkStencilFaceFlags m, uint32_t v), (c, m, v))
S0(vkCmdSetStencilCompareMask, (VkCommandBuffer c, VkStencilFaceFlags m, uint32_t v), (c, m, v))
S0(vkCmdSetStencilWriteMask, (VkCommandBuffer c, VkStencilFaceFlags m, uint32_t v), (c, m, v))
S0(vkCmdFillBuffer, (VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkDeviceSize s, uint32_t d), (c, b, o, s, d))
S0(vkCmdWriteTimestamp, (VkCommandBuffer c, VkPipelineStageFlagBits s, VkQueryPool p, uint32_t q), (c, s, p, q))
S0(vkCmdResetQueryPool, (VkCommandBuffer c, VkQueryPool p, uint32_t a, uint32_t b), (c, p, a, b))
static VKAPI_ATTR VkResult VKAPI_CALL s_vkEndCommandBuffer(VkCommandBuffer c) { rec_vals([](VkCommandBuffer x) { r_vkEndCommandBuffer(x); }, c); return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL s_vkResetCommandBuffer(VkCommandBuffer c, VkCommandBufferResetFlags f) { rec_vals([](VkCommandBuffer x, VkCommandBufferResetFlags y) { r_vkResetCommandBuffer(x, y); }, c, f); return VK_SUCCESS; }
static VKAPI_ATTR void VKAPI_CALL s_EndRendering(VkCommandBuffer c) { rec_vals([](VkCommandBuffer x) { r_EndRendering(x); }, c); }
static VKAPI_ATTR void VKAPI_CALL s_DrawByteCount(VkCommandBuffer c, uint32_t a, uint32_t b, VkBuffer d, VkDeviceSize e, uint32_t f, uint32_t g) { rec_vals(r_DrawByteCount, c, a, b, d, e, f, g); }

// stubs with arrays / structs
static VKAPI_ATTR void VKAPI_CALL s_vkCmdBindDescriptorSets(VkCommandBuffer c, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n, const VkDescriptorSet *sets, uint32_t dn, const uint32_t *dyn) {
    struct P { VkCommandBuffer c; VkPipelineBindPoint bp; VkPipelineLayout l; uint32_t first, n, dn; const VkDescriptorSet *sets; const uint32_t *dyn; };
    uint8_t *m = rec_begin(RA(sizeof(P)) + AS(sets, n) + AS(dyn, dn)); Pack pk{m + RA(sizeof(P))};
    P *p = new (m) P{c, bp, l, first, n, dn, nullptr, nullptr}; p->sets = pk.arr(sets, n); p->dyn = pk.arr(dyn, dn);
    rec_end([](uint8_t *q) { P *p = (P *)q; r_vkCmdBindDescriptorSets(p->c, p->bp, p->l, p->first, p->n, p->sets, p->dn, p->dyn); });
}
static VKAPI_ATTR void VKAPI_CALL s_vkCmdPushConstants(VkCommandBuffer c, VkPipelineLayout l, VkShaderStageFlags st, uint32_t off, uint32_t size, const void *v) {
    struct P { VkCommandBuffer c; VkPipelineLayout l; VkShaderStageFlags st; uint32_t off, size; const uint8_t *v; };
    uint8_t *m = rec_begin(RA(sizeof(P)) + AS((const uint8_t *)v, size)); Pack pk{m + RA(sizeof(P))};
    P *p = new (m) P{c, l, st, off, size, nullptr}; p->v = pk.arr((const uint8_t *)v, size);
    rec_end([](uint8_t *q) { P *p = (P *)q; r_vkCmdPushConstants(p->c, p->l, p->st, p->off, p->size, p->v); });
}
static VKAPI_ATTR void VKAPI_CALL s_vkCmdSetViewport(VkCommandBuffer c, uint32_t first, uint32_t n, const VkViewport *v) {
    struct P { VkCommandBuffer c; uint32_t first, n; const VkViewport *v; };
    uint8_t *m = rec_begin(RA(sizeof(P)) + AS(v, n)); Pack pk{m + RA(sizeof(P))};
    P *p = new (m) P{c, first, n, nullptr}; p->v = pk.arr(v, n);
    rec_end([](uint8_t *q) { P *p = (P *)q; r_vkCmdSetViewport(p->c, p->first, p->n, p->v); });
}
static VKAPI_ATTR void VKAPI_CALL s_vkCmdSetScissor(VkCommandBuffer c, uint32_t first, uint32_t n, const VkRect2D *v) {
    struct P { VkCommandBuffer c; uint32_t first, n; const VkRect2D *v; };
    uint8_t *m = rec_begin(RA(sizeof(P)) + AS(v, n)); Pack pk{m + RA(sizeof(P))};
    P *p = new (m) P{c, first, n, nullptr}; p->v = pk.arr(v, n);
    rec_end([](uint8_t *q) { P *p = (P *)q; r_vkCmdSetScissor(p->c, p->first, p->n, p->v); });
}
static VKAPI_ATTR void VKAPI_CALL s_vkCmdSetBlendConstants(VkCommandBuffer c, const float b[4]) {
    struct P { VkCommandBuffer c; float b[4]; };
    P *p = new (rec_begin(sizeof(P))) P{c, {b[0], b[1], b[2], b[3]}};
    (void)p; rec_end([](uint8_t *q) { P *p = (P *)q; r_vkCmdSetBlendConstants(p->c, p->b); });
}
static VKAPI_ATTR void VKAPI_CALL s_vkCmdBindVertexBuffers(VkCommandBuffer c, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o) {
    struct P { VkCommandBuffer c; uint32_t first, n; const VkBuffer *b; const VkDeviceSize *o; };
    uint8_t *m = rec_begin(RA(sizeof(P)) + AS(b, n) + AS(o, n)); Pack pk{m + RA(sizeof(P))};
    P *p = new (m) P{c, first, n, nullptr, nullptr}; p->b = pk.arr(b, n); p->o = pk.arr(o, n);
    rec_end([](uint8_t *q) { P *p = (P *)q; r_vkCmdBindVertexBuffers(p->c, p->first, p->n, p->b, p->o); });
}
static VKAPI_ATTR void VKAPI_CALL s_vkCmdCopyBuffer(VkCommandBuffer c, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy *r) {
    struct P { VkCommandBuffer c; VkBuffer s, d; uint32_t n; const VkBufferCopy *r; };
    uint8_t *m = rec_begin(RA(sizeof(P)) + AS(r, n)); Pack pk{m + RA(sizeof(P))};
    P *p = new (m) P{c, s, d, n, nullptr}; p->r = pk.arr(r, n);
    rec_end([](uint8_t *q) { P *p = (P *)q; r_vkCmdCopyBuffer(p->c, p->s, p->d, p->n, p->r); });
}
static VKAPI_ATTR void VKAPI_CALL s_vkCmdCopyImage(VkCommandBuffer c, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageCopy *r) {
    struct P { VkCommandBuffer c; VkImage s; VkImageLayout sl; VkImage d; VkImageLayout dl; uint32_t n; const VkImageCopy *r; };
    uint8_t *m = rec_begin(RA(sizeof(P)) + AS(r, n)); Pack pk{m + RA(sizeof(P))};
    P *p = new (m) P{c, s, sl, d, dl, n, nullptr}; p->r = pk.arr(r, n);
    rec_end([](uint8_t *q) { P *p = (P *)q; r_vkCmdCopyImage(p->c, p->s, p->sl, p->d, p->dl, p->n, p->r); });
}
static VKAPI_ATTR void VKAPI_CALL s_vkCmdCopyBufferToImage(VkCommandBuffer c, VkBuffer s, VkImage d, VkImageLayout dl, uint32_t n, const VkBufferImageCopy *r) {
    struct P { VkCommandBuffer c; VkBuffer s; VkImage d; VkImageLayout dl; uint32_t n; const VkBufferImageCopy *r; };
    uint8_t *m = rec_begin(RA(sizeof(P)) + AS(r, n)); Pack pk{m + RA(sizeof(P))};
    P *p = new (m) P{c, s, d, dl, n, nullptr}; p->r = pk.arr(r, n);
    rec_end([](uint8_t *q) { P *p = (P *)q; r_vkCmdCopyBufferToImage(p->c, p->s, p->d, p->dl, p->n, p->r); });
}
static VKAPI_ATTR void VKAPI_CALL s_vkCmdCopyImageToBuffer(VkCommandBuffer c, VkImage s, VkImageLayout sl, VkBuffer d, uint32_t n, const VkBufferImageCopy *r) {
    struct P { VkCommandBuffer c; VkImage s; VkImageLayout sl; VkBuffer d; uint32_t n; const VkBufferImageCopy *r; };
    uint8_t *m = rec_begin(RA(sizeof(P)) + AS(r, n)); Pack pk{m + RA(sizeof(P))};
    P *p = new (m) P{c, s, sl, d, n, nullptr}; p->r = pk.arr(r, n);
    rec_end([](uint8_t *q) { P *p = (P *)q; r_vkCmdCopyImageToBuffer(p->c, p->s, p->sl, p->d, p->n, p->r); });
}
static VKAPI_ATTR void VKAPI_CALL s_vkCmdBlitImage(VkCommandBuffer c, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageBlit *r, VkFilter f) {
    struct P { VkCommandBuffer c; VkImage s; VkImageLayout sl; VkImage d; VkImageLayout dl; uint32_t n; const VkImageBlit *r; VkFilter f; };
    uint8_t *m = rec_begin(RA(sizeof(P)) + AS(r, n)); Pack pk{m + RA(sizeof(P))};
    P *p = new (m) P{c, s, sl, d, dl, n, nullptr, f}; p->r = pk.arr(r, n);
    rec_end([](uint8_t *q) { P *p = (P *)q; r_vkCmdBlitImage(p->c, p->s, p->sl, p->d, p->dl, p->n, p->r, p->f); });
}
static VKAPI_ATTR void VKAPI_CALL s_vkCmdClearColorImage(VkCommandBuffer c, VkImage im, VkImageLayout l, const VkClearColorValue *v, uint32_t n, const VkImageSubresourceRange *r) {
    struct P { VkCommandBuffer c; VkImage im; VkImageLayout l; VkClearColorValue v; uint32_t n; const VkImageSubresourceRange *r; };
    uint8_t *m = rec_begin(RA(sizeof(P)) + AS(r, n)); Pack pk{m + RA(sizeof(P))};
    P *p = new (m) P{c, im, l, *v, n, nullptr}; p->r = pk.arr(r, n);
    rec_end([](uint8_t *q) { P *p = (P *)q; r_vkCmdClearColorImage(p->c, p->im, p->l, &p->v, p->n, p->r); });
}
static VKAPI_ATTR void VKAPI_CALL s_vkCmdClearDepthStencilImage(VkCommandBuffer c, VkImage im, VkImageLayout l, const VkClearDepthStencilValue *v, uint32_t n, const VkImageSubresourceRange *r) {
    struct P { VkCommandBuffer c; VkImage im; VkImageLayout l; VkClearDepthStencilValue v; uint32_t n; const VkImageSubresourceRange *r; };
    uint8_t *m = rec_begin(RA(sizeof(P)) + AS(r, n)); Pack pk{m + RA(sizeof(P))};
    P *p = new (m) P{c, im, l, *v, n, nullptr}; p->r = pk.arr(r, n);
    rec_end([](uint8_t *q) { P *p = (P *)q; r_vkCmdClearDepthStencilImage(p->c, p->im, p->l, &p->v, p->n, p->r); });
}
static VKAPI_ATTR void VKAPI_CALL s_vkCmdPipelineBarrier(VkCommandBuffer c, VkPipelineStageFlags ss, VkPipelineStageFlags ds, VkDependencyFlags df,
                                                         uint32_t mn, const VkMemoryBarrier *mb, uint32_t bn, const VkBufferMemoryBarrier *bb, uint32_t in, const VkImageMemoryBarrier *ib) {
    struct P { VkCommandBuffer c; VkPipelineStageFlags ss, ds; VkDependencyFlags df; uint32_t mn, bn, in; const VkMemoryBarrier *mb; const VkBufferMemoryBarrier *bb; const VkImageMemoryBarrier *ib; };
    uint8_t *m = rec_begin(RA(sizeof(P)) + AS(mb, mn) + AS(bb, bn) + AS(ib, in)); Pack pk{m + RA(sizeof(P))};
    P *p = new (m) P{c, ss, ds, df, mn, bn, in, nullptr, nullptr, nullptr}; p->mb = pk.arr(mb, mn); p->bb = pk.arr(bb, bn); p->ib = pk.arr(ib, in);
    rec_end([](uint8_t *q) { P *p = (P *)q; r_vkCmdPipelineBarrier(p->c, p->ss, p->ds, p->df, p->mn, p->mb, p->bn, p->bb, p->in, p->ib); });
}
static VKAPI_ATTR void VKAPI_CALL s_BeginRendering(VkCommandBuffer c, const VkRenderingInfo *ri) {
    struct P { VkCommandBuffer c; VkRenderingInfo ri; VkRenderingAttachmentInfo d, s; };
    uint8_t *m = rec_begin(RA(sizeof(P)) + AS(ri->pColorAttachments, ri->colorAttachmentCount)); Pack pk{m + RA(sizeof(P))};
    P *p = new (m) P{c, *ri, {}, {}};
    p->ri.pNext = nullptr;
    p->ri.pColorAttachments = pk.arr(ri->pColorAttachments, ri->colorAttachmentCount);
    if (ri->pDepthAttachment) { p->d = *ri->pDepthAttachment; p->ri.pDepthAttachment = &p->d; }
    if (ri->pStencilAttachment) { p->s = *ri->pStencilAttachment; p->ri.pStencilAttachment = &p->s; }
    rec_end([](uint8_t *q) { P *p = (P *)q; r_BeginRendering(p->c, &p->ri); });
}
static VKAPI_ATTR void VKAPI_CALL s_BindXfb(VkCommandBuffer c, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o, const VkDeviceSize *sz) {
    struct P { VkCommandBuffer c; uint32_t first, n; const VkBuffer *b; const VkDeviceSize *o, *sz; };
    uint8_t *m = rec_begin(RA(sizeof(P)) + AS(b, n) + AS(o, n) + AS(sz, n)); Pack pk{m + RA(sizeof(P))};
    P *p = new (m) P{c, first, n, nullptr, nullptr, nullptr}; p->b = pk.arr(b, n); p->o = pk.arr(o, n); p->sz = pk.arr(sz, n);
    rec_end([](uint8_t *q) { P *p = (P *)q; r_BindXfb(p->c, p->first, p->n, p->b, p->o, p->sz); });
}
#define XFB_STUB(NAME, REAL) \
static VKAPI_ATTR void VKAPI_CALL NAME(VkCommandBuffer c, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o) { \
    struct P { VkCommandBuffer c; uint32_t first, n; const VkBuffer *b; const VkDeviceSize *o; }; \
    uint8_t *m = rec_begin(RA(sizeof(P)) + AS(b, n) + AS(o, n)); Pack pk{m + RA(sizeof(P))}; \
    P *p = new (m) P{c, first, n, nullptr, nullptr}; p->b = pk.arr(b, n); p->o = pk.arr(o, n); \
    rec_end([](uint8_t *q) { P *p = (P *)q; REAL(p->c, p->first, p->n, p->b, p->o); }); }
XFB_STUB(s_BeginXfb, r_BeginXfb)
XFB_STUB(s_EndXfb, r_EndXfb)
static VKAPI_ATTR VkResult VKAPI_CALL s_vkBeginCommandBuffer(VkCommandBuffer c, const VkCommandBufferBeginInfo *bi) {
    struct P { VkCommandBuffer c; VkCommandBufferBeginInfo bi; };
    P *p = new (rec_begin(sizeof(P))) P{c, *bi}; p->bi.pNext = nullptr; p->bi.pInheritanceInfo = nullptr;
    rec_end([](uint8_t *q) { P *p = (P *)q; r_vkBeginCommandBuffer(p->c, &p->bi); });
    return VK_SUCCESS;
}
// fence -> end of the record that submits it: a wait on that fence only needs the recorder that far
static struct { VkFence f; size_t pos; } g_fence_pos[32]; static unsigned g_fence_pos_n;
static void rec_wait_fences(uint32_t n, const VkFence *f) {
    if (!g_rec_on) return;
    size_t upto = 0;
    for (uint32_t i = 0; i < n; i++) {
        size_t pos = SIZE_MAX;
        for (unsigned k = 0; k < 32; k++) if (g_fence_pos[k].f == f[i]) { pos = g_fence_pos[k].pos; break; }
        if (pos == SIZE_MAX) { rec_drain(); return; }   // (not submitted through the ring that we know of)
        upto = std::max(upto, pos);
    }
    for (int i = 0; g_rtail.load(std::memory_order_acquire) < upto; i++) { if (i > 64) std::this_thread::yield(); }
}
static VKAPI_ATTR VkResult VKAPI_CALL s_vkQueueSubmit(VkQueue qu, uint32_t n, const VkSubmitInfo *si, VkFence f) {
    struct P { VkQueue q; uint32_t n; VkFence f; const VkSubmitInfo *si; };
    size_t sz = RA(sizeof(P)) + AS(si, n);
    for (uint32_t i = 0; i < n; i++) sz += AS(si[i].pWaitSemaphores, si[i].waitSemaphoreCount) + AS(si[i].pWaitDstStageMask, si[i].waitSemaphoreCount) +
                                       AS(si[i].pCommandBuffers, si[i].commandBufferCount) + AS(si[i].pSignalSemaphores, si[i].signalSemaphoreCount);
    uint8_t *m = rec_begin(sz); Pack pk{m + RA(sizeof(P))};
    P *p = new (m) P{qu, n, f, nullptr};
    VkSubmitInfo *cs = (VkSubmitInfo *)pk.arr(si, n); p->si = cs;
    for (uint32_t i = 0; i < n; i++) {
        cs[i].pNext = nullptr;
        cs[i].pWaitSemaphores = pk.arr(si[i].pWaitSemaphores, si[i].waitSemaphoreCount);
        cs[i].pWaitDstStageMask = pk.arr(si[i].pWaitDstStageMask, si[i].waitSemaphoreCount);
        cs[i].pCommandBuffers = pk.arr(si[i].pCommandBuffers, si[i].commandBufferCount);
        cs[i].pSignalSemaphores = pk.arr(si[i].pSignalSemaphores, si[i].signalSemaphoreCount);
    }
    rec_end([](uint8_t *q) { P *p = (P *)q; std::lock_guard<std::mutex> l(g_qm); r_vkQueueSubmit(p->q, p->n, p->si, p->f); });
    if (f) {
        unsigned k = 0; for (; k < 32; k++) if (g_fence_pos[k].f == f) break;
        if (k == 32) k = g_fence_pos_n++ % 32;
        g_fence_pos[k].f = f; g_fence_pos[k].pos = g_rhead.load(std::memory_order_relaxed);
    }
    return VK_SUCCESS;
}
static size_t g_present_pos;
static VKAPI_ATTR VkResult VKAPI_CALL s_vkQueuePresentKHR(VkQueue qu, const VkPresentInfoKHR *pi) {
    struct P { VkQueue q; VkPresentInfoKHR pi; };
    uint8_t *m = rec_begin(RA(sizeof(P)) + AS(pi->pWaitSemaphores, pi->waitSemaphoreCount) + AS(pi->pSwapchains, pi->swapchainCount) + AS(pi->pImageIndices, pi->swapchainCount));
    Pack pk{m + RA(sizeof(P))};
    P *p = new (m) P{qu, *pi}; p->pi.pNext = nullptr; p->pi.pResults = nullptr;
    p->pi.pWaitSemaphores = pk.arr(pi->pWaitSemaphores, pi->waitSemaphoreCount);
    p->pi.pSwapchains = pk.arr(pi->pSwapchains, pi->swapchainCount);
    p->pi.pImageIndices = pk.arr(pi->pImageIndices, pi->swapchainCount);
    rec_end([](uint8_t *q) { P *p = (P *)q; std::lock_guard<std::mutex> l(g_qm); VkResult r = r_vkQueuePresentKHR(p->q, &p->pi); if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) g_resized = true; });
    g_present_pos = g_rhead.load(std::memory_order_relaxed);
    return VK_SUCCESS;
}
// acquire and present use the swapchain from two threads: the previous present has to have been made
static VKAPI_ATTR VkResult VKAPI_CALL s_vkAcquireNextImageKHR(VkDevice d, VkSwapchainKHR sc, uint64_t t, VkSemaphore s, VkFence f, uint32_t *i) {
    for (int k = 0; g_rtail.load(std::memory_order_acquire) < g_present_pos; k++) { if (k > 64) std::this_thread::yield(); }
    return r_vkAcquireNextImageKHR(d, sc, t, s, f, i);
}
// calls that need the recorded work made first
static VKAPI_ATTR VkResult VKAPI_CALL s_vkWaitForFences(VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t t) { rec_wait_fences(n, f); return r_vkWaitForFences(d, n, f, all, t); }
static VKAPI_ATTR VkResult VKAPI_CALL s_vkResetFences(VkDevice d, uint32_t n, const VkFence *f) { rec_wait_fences(n, f); return r_vkResetFences(d, n, f); }
// (query results are read after waiting for the fence of the work that wrote them)
static VKAPI_ATTR VkResult VKAPI_CALL s_vkGetQueryPoolResults(VkDevice d, VkQueryPool p, uint32_t a, uint32_t b, size_t s, void *v, VkDeviceSize st, VkQueryResultFlags fl) { return r_vkGetQueryPoolResults(d, p, a, b, s, v, st, fl); }
static VKAPI_ATTR VkResult VKAPI_CALL s_vkDeviceWaitIdle(VkDevice d) { pacer_flush(); rec_drain(); std::lock_guard<std::mutex> l(g_qm); return r_vkDeviceWaitIdle(d); }
static VKAPI_ATTR VkResult VKAPI_CALL s_vkAllocateCommandBuffers(VkDevice d, const VkCommandBufferAllocateInfo *ai, VkCommandBuffer *cb) { rec_drain(); return r_vkAllocateCommandBuffers(d, ai, cb); }   // (the pool is in use by the recorder)
static VKAPI_ATTR void VKAPI_CALL s_vkDestroyImage(VkDevice d, VkImage i, const VkAllocationCallbacks *a) { rec_drain(); r_vkDestroyImage(d, i, a); }
static VKAPI_ATTR void VKAPI_CALL s_vkDestroyImageView(VkDevice d, VkImageView i, const VkAllocationCallbacks *a) { rec_drain(); r_vkDestroyImageView(d, i, a); }
static VKAPI_ATTR void VKAPI_CALL s_vkDestroyBuffer(VkDevice d, VkBuffer b, const VkAllocationCallbacks *a) { rec_drain(); r_vkDestroyBuffer(d, b, a); }
static VKAPI_ATTR void VKAPI_CALL s_vkFreeMemory(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a) { rec_drain(); r_vkFreeMemory(d, m, a); }
static VKAPI_ATTR void VKAPI_CALL s_vkDestroySwapchainKHR(VkDevice d, VkSwapchainKHR s, const VkAllocationCallbacks *a) { rec_drain(); r_vkDestroySwapchainKHR(d, s, a); }

// ---------------------------------------------------------------- frame-generation pacer (gpu_fg.inc)
// With frame generation each game frame shows two images: the generated one at once, the rendered one half a frame
// later. This thread makes those presents (after the recorder has made the frame's submit) at those times, so the
// recorder never sleeps. The queue and the swapchain are used from three threads then: every real submit / present /
// acquire / wait-idle holds g_qm.
struct PacerJob { size_t pos; VkSwapchainKHR sc; uint32_t idx[2]; VkSemaphore sem[2]; int n; uint64_t gap_ns; };
static std::mutex g_pm; static std::condition_variable g_pcv;
static std::vector<PacerJob> g_pjobs; static bool g_pbusy;
static std::atomic<uint64_t> g_pacer_presents{0};
static void pacer_thread() {
    xrt_sampler_register_thread("frame pacer");
    for (;;) {
        PacerJob j;
        {
            std::unique_lock<std::mutex> l(g_pm);
            g_pcv.wait(l, [] { return !g_pjobs.empty(); });
            j = g_pjobs.front(); g_pjobs.erase(g_pjobs.begin()); g_pbusy = true;
        }
        for (int i = 0; g_rtail.load(std::memory_order_acquire) < j.pos; i++) { if (i > 64) std::this_thread::yield(); }
        uint64_t t0 = 0;
        for (int k = 0; k < j.n; k++) {
            if (k == 1) plat_sleep_until_ns(t0 + j.gap_ns);
            VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &j.sem[k];
            pi.swapchainCount = 1; pi.pSwapchains = &j.sc; pi.pImageIndices = &j.idx[k];
            VkResult r;
            { std::lock_guard<std::mutex> l(g_qm); r = (g_rec_on ? r_vkQueuePresentKHR : vkQueuePresentKHR)(g_queue, &pi); }
            if (k == 0) t0 = plat_ticks_ns();
            if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) g_resized = true;
            g_pacer_presents++;
        }
        { std::lock_guard<std::mutex> l(g_pm); g_pbusy = false; }
        g_pcv.notify_all();
    }
}
static void pacer_submit(const PacerJob &j) {
    static bool started;
    if (!started) { started = true; std::thread(pacer_thread).detach(); }
    { std::lock_guard<std::mutex> l(g_pm); g_pjobs.push_back(j); }
    g_pcv.notify_all();
}
static void pacer_flush() {   // every present handed over has been made
    std::unique_lock<std::mutex> l(g_pm);
    g_pcv.wait(l, [] { return g_pjobs.empty() && !g_pbusy; });
}

static void rec_install() {
    const char *e = getenv("XRT_RECORD_THREAD");
    if (e && atoi(e) == 0) { LOG(1, "[gpu] command recording on the render worker (XRT_RECORD_THREAD=0)"); return; }
    g_rb = (uint8_t *)plat_alloc(RB_SIZE);
#define SWAP(f) r_##f = f; f = s_##f;
    SWAP(vkCmdBindPipeline) SWAP(vkCmdBindIndexBuffer) SWAP(vkCmdDraw) SWAP(vkCmdDrawIndexed) SWAP(vkCmdSetDepthBias)
    SWAP(vkCmdSetStencilReference) SWAP(vkCmdSetStencilCompareMask) SWAP(vkCmdSetStencilWriteMask) SWAP(vkCmdFillBuffer)
    SWAP(vkCmdWriteTimestamp) SWAP(vkCmdResetQueryPool) SWAP(vkEndCommandBuffer) SWAP(vkResetCommandBuffer)
    SWAP(vkCmdBindDescriptorSets) SWAP(vkCmdPushConstants) SWAP(vkCmdSetViewport) SWAP(vkCmdSetScissor) SWAP(vkCmdSetBlendConstants)
    SWAP(vkCmdBindVertexBuffers) SWAP(vkCmdCopyBuffer) SWAP(vkCmdCopyImage) SWAP(vkCmdCopyBufferToImage) SWAP(vkCmdCopyImageToBuffer)
    SWAP(vkCmdBlitImage) SWAP(vkCmdClearColorImage) SWAP(vkCmdClearDepthStencilImage) SWAP(vkCmdPipelineBarrier)
    SWAP(vkBeginCommandBuffer) SWAP(vkQueueSubmit) SWAP(vkQueuePresentKHR)
    SWAP(vkWaitForFences) SWAP(vkResetFences) SWAP(vkGetQueryPoolResults) SWAP(vkDeviceWaitIdle) SWAP(vkAllocateCommandBuffers)
    SWAP(vkAcquireNextImageKHR) SWAP(vkDestroyImage) SWAP(vkDestroyImageView) SWAP(vkDestroyBuffer) SWAP(vkFreeMemory) SWAP(vkDestroySwapchainKHR)
#undef SWAP
    if (pfnCmdBeginRendering) { r_BeginRendering = pfnCmdBeginRendering; pfnCmdBeginRendering = s_BeginRendering; r_EndRendering = pfnCmdEndRendering; pfnCmdEndRendering = s_EndRendering; }
    if (pfnCmdDrawByteCount) { r_DrawByteCount = pfnCmdDrawByteCount; pfnCmdDrawByteCount = s_DrawByteCount; }
    if (pfnCmdBindXfb) { r_BindXfb = pfnCmdBindXfb; pfnCmdBindXfb = s_BindXfb; }
    if (pfnCmdBeginXfb) { r_BeginXfb = pfnCmdBeginXfb; pfnCmdBeginXfb = s_BeginXfb; }
    if (pfnCmdEndXfb) { r_EndXfb = pfnCmdEndXfb; pfnCmdEndXfb = s_EndXfb; }
    g_rec_on = true;
    std::thread(rec_thread).detach();
    LOG(1, "[gpu] command recording thread on (XRT_RECORD_THREAD=0 to record on the render worker)");
}
