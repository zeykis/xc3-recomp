// nn::image::JpegDecoder (Nintendo SDK image library), implemented with stb_image (public domain).
//
// The game decodes JPEG pictures from its own data (e.g. loading/menu images). The decoder object lives
// in guest memory but we never touch its contents: state is kept in a side table keyed by its address.
//   SetImageData(data, size), SetPixelFormat(fmt), SetResolutionDenominator(n), Analyze() -> JpegStatus,
//   GetAnalyzedDimension() -> {width, height}, GetAnalyzedWorkBufferSize(), Decode(out, outSize, lineAlignment, work, workSize)
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb/stb_image.h"
extern "C" {
#include "xrt_internal.h"
}
#define P(i) ((void *)(uintptr_t)c->x[i])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define HLE(name) static void name(Ctx *c)

// nn::image::JpegStatus
enum { JPEG_OK = 0, JPEG_INVALID_FORMAT = -32, JPEG_UNSUPPORTED = -33, JPEG_OUT_OF_MEMORY = -64 };

struct Jpeg { const uint8_t *data = nullptr; size_t size = 0; int format = 0, denom = 1, w = 0, h = 0; bool analyzed = false; };
static std::mutex g_jl;
static std::unordered_map<uint64_t, Jpeg> g_jpegs;
static Jpeg &J(uint64_t a) { return g_jpegs[a]; }

// JpegDecoder is polymorphic (the SDK has _ZTVN2nn5image11JpegDecoderE with a deleting destructor):
// the constructor must store a vtable pointer in the object, because the game destroys decoders through
// it. Slots: [0] complete destructor (D1), [1] deleting destructor (D0). The deleting destructor doesn't
// free the guest memory (we don't know the allocator it came from): a few hundred bytes per decoded image.
static uint64_t g_jd_vt;
HLE(jd_dtor) { std::lock_guard<std::mutex> l(g_jl); g_jpegs.erase(c->x[0]); }
HLE(jd_ctor) {
    std::lock_guard<std::mutex> l(g_jl);
    if (!g_jd_vt) {
        uint64_t *vt = (uint64_t *)plat_alloc(sizeof(uint64_t) * 4);
        vt[0] = 0; vt[1] = 0;                                   // offset-to-top, type info
        vt[2] = hle_resolve("image:JpegDecoder::D1"); vt[3] = hle_resolve("image:JpegDecoder::D0");
        g_jd_vt = (uint64_t)(uintptr_t)(vt + 2);
    }
    *(uint64_t *)P(0) = g_jd_vt;
    g_jpegs[c->x[0]] = Jpeg{};
}
HLE(jd_set_data) { std::lock_guard<std::mutex> l(g_jl); Jpeg &j = J(c->x[0]); j.data = (const uint8_t *)P(1); j.size = (size_t)c->x[2]; j.analyzed = false; }
HLE(jd_set_format) { std::lock_guard<std::mutex> l(g_jl); J(c->x[0]).format = (int)(uint32_t)c->x[1]; }
HLE(jd_set_denom) { std::lock_guard<std::mutex> l(g_jl); int d = (int)(uint32_t)c->x[1]; J(c->x[0]).denom = d == 2 || d == 4 || d == 8 ? d : 1; }
HLE(jd_analyze) {
    std::lock_guard<std::mutex> l(g_jl); Jpeg &j = J(c->x[0]);
    int w, h, comp;
    if (!j.data || !j.size || !stbi_info_from_memory(j.data, (int)j.size, &w, &h, &comp)) {
        LOG(1, "[image] JpegDecoder::Analyze: not a decodable JPEG (%zu bytes)", j.size); RET((uint32_t)JPEG_INVALID_FORMAT); return;
    }
    j.w = (w + j.denom - 1) / j.denom; j.h = (h + j.denom - 1) / j.denom; j.analyzed = true;
    RET(JPEG_OK);
}
HLE(jd_dimension) {      // returns nn::image::Dimension {int32 width, int32 height} in x0
    std::lock_guard<std::mutex> l(g_jl); Jpeg &j = J(c->x[0]);
    RET((uint64_t)(uint32_t)j.w | (uint64_t)(uint32_t)j.h << 32);
}
HLE(jd_work_size) { RET(4096); }   // stb_image allocates internally; the game only needs a non-zero buffer
HLE(jd_decode) {         // (this, void* out, size_t outSize, int lineAlignment, void* work, size_t workSize)
    std::lock_guard<std::mutex> l(g_jl); Jpeg &j = J(c->x[0]);
    if (!j.analyzed) { RET((uint32_t)JPEG_INVALID_FORMAT); return; }
    uint8_t *out = (uint8_t *)P(1); size_t out_size = (size_t)c->x[2]; int align = (int)(uint32_t)c->x[3];
    // nn::image::PixelFormat's numbering isn't confirmed, so pick the layout the caller's buffer was sized
    // for: RGBA32 when it fits, else RGB24
    if (align < 1) align = 1;
    int ow = j.w, oh = j.h;
    size_t aw = (size_t)((ow + align - 1) / align * align);
    int bpp = out_size >= aw * 4 * (size_t)oh ? 4 : 3;
    size_t pitch = aw * bpp;
    int w, h, comp;
    uint8_t *px = stbi_load_from_memory(j.data, (int)j.size, &w, &h, &comp, bpp);
    if (!px) { LOG(1, "[image] JpegDecoder::Decode failed: %s", stbi_failure_reason()); RET((uint32_t)JPEG_INVALID_FORMAT); return; }
    if (!out || out_size < pitch * (size_t)oh) {
        LOG(1, "[image] JpegDecoder::Decode: output buffer too small (%zu < %zu)", out_size, pitch * (size_t)oh);
        stbi_image_free(px); RET((uint32_t)JPEG_OUT_OF_MEMORY); return;
    }
    for (int y = 0; y < oh; y++)          // point-sample when a resolution denominator was set
        for (int x = 0; x < ow; x++) {
            int sx = x * j.denom < w ? x * j.denom : w - 1, sy = y * j.denom < h ? y * j.denom : h - 1;
            memcpy(out + (size_t)y * pitch + (size_t)x * bpp, px + ((size_t)sy * w + sx) * bpp, bpp);
        }
    stbi_image_free(px);
    static int logged; if (logged++ < 10) LOG(1, "[image] decoded JPEG %dx%d (format %d -> %d bytes/pixel, align %d)", ow, oh, j.format, bpp, align);
    RET(JPEG_OK);
}

#define E(n, f) {n, f}
extern "C" const HleImpl hle_image[] = {
    E("_ZN2nn5image11JpegDecoderC1Ev", jd_ctor), E("_ZN2nn5image11JpegDecoderC2Ev", jd_ctor),
    E("_ZN2nn5image11JpegDecoderD1Ev", jd_dtor), E("_ZN2nn5image11JpegDecoderD2Ev", jd_dtor),
    E("_ZN2nn5image11JpegDecoderD0Ev", jd_dtor), E("image:JpegDecoder::D1", jd_dtor), E("image:JpegDecoder::D0", jd_dtor),
    E("_ZN2nn5image11JpegDecoder12SetImageDataEPKvm", jd_set_data),
    E("_ZN2nn5image11JpegDecoder14SetPixelFormatENS0_11PixelFormatE", jd_set_format),
    E("_ZN2nn5image11JpegDecoder24SetResolutionDenominatorEi", jd_set_denom),
    E("_ZN2nn5image11JpegDecoder7AnalyzeEv", jd_analyze),
    E("_ZNK2nn5image11JpegDecoder20GetAnalyzedDimensionEv", jd_dimension),
    E("_ZNK2nn5image11JpegDecoder25GetAnalyzedWorkBufferSizeEv", jd_work_size),
    E("_ZN2nn5image11JpegDecoder6DecodeEPvmiS2_m", jd_decode),
    {nullptr, nullptr}
};
