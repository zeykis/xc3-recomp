// movie:: (Nintendo multimedia library in subsdk0) with FFmpeg, + nn::os message queues.
//
// The game's cutscenes are WebM / VP9 (romfs/movie/*.webm, 1280x720, 30 fps, no audio track: their sound is
// Wwise's). The game drives an Android MediaExtractor / MediaCodec-style API: an Extractor demuxes the file into
// samples, a Decoder takes them in input buffers and hands decoded frames back in output buffers, signalling the
// game's events (input available, output available, ...). Both are replaced here: libavformat demuxes, libavcodec
// decodes (FFmpeg's native VP9 decoder), frames are handed over as NV12.
//
// Design and many details follow Monado's movie HLE (XCDE recomp, MIT; itself from LASRecomp): MediaData as an
// 8-byte handle, Buffer as <= 0x40 bytes of fields, output backpressure, one end-of-stream output per stream, no
// events after Stop. Vtable slots are XC3's own (subsdk0.elf _ZTVN5movie9ExtractorE / _ZTVN5movie7DecoderE).
//
// XRT_NO_MOVIE=1: report that files can't be opened (the game skips its videos, the old behaviour).
// XRT_LOG_MOVIE=1: log the calls the game makes (keys it looks up, buffers it passes).
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
extern "C" {
#include "xrt_internal.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
}
#define P(i) ((void *)(uintptr_t)c->x[i])
#define S(i) ((const char *)(uintptr_t)c->x[i])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define HLE(name) static void name(Ctx *c)

void xrt_os_signal_event(uint64_t ev);      // hle_nn.cpp
void xrt_os_clear_event(uint64_t ev);
std::string xrt_host_path(const char *guest_path);

static const int MOVIE_OK = 0, MOVIE_ERROR = 1;
static const uint32_t MOVIE_EOS = 4;        // buffer flag: end of stream
static std::mutex g_ml;

static bool movie_log() { static int v = -1; if (v < 0) v = getenv("XRT_LOG_MOVIE") ? 1 : 0; return v != 0; }
#define MLOG(...) do { if (movie_log()) LOG(0, __VA_ARGS__); } while (0)
// XRT_LOG_MOVIE: the first 40 calls of each Extractor / Decoder method, with their raw arguments
static void mtrace(const char *fn, Ctx *c) {
    if (!movie_log()) return;
    static std::mutex m; static std::unordered_map<std::string, int> n;
    { std::lock_guard<std::mutex> l(m); if (n[fn]++ >= 40) return; }
    LOG(0, "[movie] > %s(%llx, %llx, %llx, %llx, %llx, %llx) lr %llx", fn, (unsigned long long)c->x[0], (unsigned long long)c->x[1],
        (unsigned long long)c->x[2], (unsigned long long)c->x[3], (unsigned long long)c->x[4], (unsigned long long)c->x[5], (unsigned long long)c->x[30]);
}

// ---------------------------------------------------------------- MediaData (guest object: +0 = host pointer)
struct MdEntry { std::string key, sval; int64_t ival = 0; bool is_str = false; };
struct MediaData {
    std::vector<MdEntry> e;
    std::shared_ptr<AVCodecParameters> par;   // a Configure() source: the track's codec parameters
};
static MediaData *md(uint64_t guest) {
    MediaData **p = (MediaData **)(uintptr_t)guest;
    if (!*p) *p = new MediaData();
    return *p;
}
static MdEntry &md_slot(MediaData *m, const char *k) {
    for (auto &x : m->e) if (x.key == k) return x;
    m->e.push_back(MdEntry{k});
    return m->e.back();
}
static void md_set_int(MediaData *m, const char *k, int64_t v) { MdEntry &x = md_slot(m, k); x.ival = v; x.is_str = false; }
static void md_set_str(MediaData *m, const char *k, const char *v) { MdEntry &x = md_slot(m, k); x.sval = v; x.is_str = true; }

HLE(md_ctor) { *(uint64_t *)P(0) = (uint64_t)(uintptr_t)new MediaData(); }
HLE(md_copy) { *(uint64_t *)P(0) = (uint64_t)(uintptr_t)new MediaData(*md(c->x[1])); }
HLE(md_assign) { MediaData *d = md(c->x[0]), *s = md(c->x[1]); if (d != s) *d = *s; }   // returns *this (x0)
HLE(md_dtor) { MediaData **p = (MediaData **)P(0); delete *p; *p = nullptr; }
HLE(md_clear) { MediaData *m = md(c->x[0]); m->e.clear(); m->par.reset(); }
HLE(md_set_int32) { md_set_int(md(c->x[0]), S(1), (int32_t)c->x[2]); MLOG("[movie] MediaData::SetInt32 %s = %d", S(1), (int)(int32_t)c->x[2]); }
HLE(md_find_int32) {
    MediaData *m = md(c->x[0]);
    for (auto &x : m->e)
        if (!x.is_str && x.key == S(1)) { *(int32_t *)P(2) = (int32_t)x.ival; MLOG("[movie] FindInt32 %s = %d", S(1), (int)x.ival); RET(1); return; }
    MLOG("[movie] FindInt32 %s: not found", S(1));
    RET(0);
}
HLE(md_find_string) {
    MediaData *m = md(c->x[0]);
    for (auto &x : m->e)
        if (x.is_str && x.key == S(1)) { *(uint64_t *)P(2) = (uint64_t)(uintptr_t)x.sval.c_str(); MLOG("[movie] FindString %s = %s", S(1), x.sval.c_str()); RET(1); return; }
    MLOG("[movie] FindString %s: not found", S(1));
    RET(0);
}

// ---------------------------------------------------------------- Buffer (guest object, our fields)
struct MBuffer { uint64_t base, capacity; int32_t offset, size; int64_t i64; uint32_t u32; int32_t i32; };
static_assert(sizeof(MBuffer) <= 0x28, "movie::Buffer fields");
#define BUF(x) ((MBuffer *)(uintptr_t)(x))
HLE(buf_ctor) { std::memset(BUF(c->x[0]), 0, sizeof(MBuffer)); }
HLE(buf_ctor_ext) { MBuffer *b = BUF(c->x[0]); std::memset(b, 0, sizeof *b); b->base = c->x[1]; b->capacity = c->x[2]; b->size = (int32_t)c->x[2]; }
HLE(buf_ctor_alloc) {
    MBuffer *b = BUF(c->x[0]); std::memset(b, 0, sizeof *b);
    b->base = (uint64_t)(uintptr_t)plat_alloc(c->x[1] + 64); b->capacity = c->x[1]; b->size = (int32_t)c->x[1];
}
HLE(buf_copy) { *BUF(c->x[0]) = *BUF(c->x[1]); }
HLE(buf_dtor) { }
HLE(buf_base) { RET(BUF(c->x[0])->base); }
HLE(buf_size) { RET((uint32_t)BUF(c->x[0])->size); }
HLE(buf_offset) { RET((uint32_t)BUF(c->x[0])->offset); }
HLE(buf_capacity) { RET(BUF(c->x[0])->capacity); }
HLE(buf_set_range) { BUF(c->x[0])->offset = (int32_t)c->x[1]; BUF(c->x[0])->size = (int32_t)c->x[2]; }
HLE(buf_set_data) { MBuffer *b = BUF(c->x[0]); b->base = c->x[1]; b->capacity = c->x[2]; }
HLE(buf_set_i64) { BUF(c->x[0])->i64 = (int64_t)c->x[1]; }
HLE(buf_get_i64) { RET((uint64_t)BUF(c->x[0])->i64); }
HLE(buf_set_u32) { BUF(c->x[0])->u32 = (uint32_t)c->x[1]; }
HLE(buf_get_u32) { RET(BUF(c->x[0])->u32); }
HLE(buf_set_i32) { BUF(c->x[0])->i32 = (int32_t)c->x[1]; }
HLE(m_ok) { RET(MOVIE_OK); }
HLE(m_prio) { RET(16); }

// ---------------------------------------------------------------- vtables
static uint64_t make_vtable(const char *const *slots, int n) {
    uint64_t *vt = (uint64_t *)plat_alloc(sizeof(uint64_t) * (n + 2));
    for (int i = 0; i < n; i++) vt[2 + i] = hle_resolve(slots[i]);
    return (uint64_t)(uintptr_t)(vt + 2);
}
template <class T> static T *self(Ctx *c) { return (T *)(uintptr_t)((uint64_t *)P(0))[1]; }

// ---------------------------------------------------------------- Extractor
struct Extractor {
    AVFormatContext *fmt = nullptr;
    int map[16] = {}, ntracks = 0;          // exposed track -> stream
    uint64_t selected = 0;                  // bitmask of streams
    AVPacket *pkt = nullptr;
    bool have_pkt = false, eof = false;
};
static std::mutex g_ex_lock;                // libavformat contexts aren't thread-safe; the game reads from 2 threads

static void ex_fill(Extractor *e) {
    while (e->fmt && !e->have_pkt && !e->eof) {
        if (av_read_frame(e->fmt, e->pkt) < 0) { e->eof = true; break; }
        if (e->selected & (1ull << e->pkt->stream_index)) e->have_pkt = true;
        else av_packet_unref(e->pkt);
    }
}
static int ex_open(Extractor *e, const char *guest_path) {
    static int off = -1; if (off < 0) off = getenv("XRT_NO_MOVIE") ? 1 : 0;
    std::string host = guest_path ? xrt_host_path(guest_path) : std::string();
    if (off) { LOG(1, "[movie] %s: skipped (XRT_NO_MOVIE)", guest_path ? guest_path : "?"); return MOVIE_ERROR; }
    if (avformat_open_input(&e->fmt, host.c_str(), nullptr, nullptr) < 0 || avformat_find_stream_info(e->fmt, nullptr) < 0) {
        LOG(0, "[movie] cannot open %s (%s)", guest_path ? guest_path : "?", host.c_str());
        if (e->fmt) avformat_close_input(&e->fmt);
        return MOVIE_ERROR;
    }
    e->pkt = av_packet_alloc();
    e->ntracks = 0;
    for (unsigned i = 0; i < e->fmt->nb_streams && e->ntracks < 16; i++) {
        AVMediaType t = e->fmt->streams[i]->codecpar->codec_type;
        if (t == AVMEDIA_TYPE_VIDEO || t == AVMEDIA_TYPE_AUDIO) e->map[e->ntracks++] = (int)i;
    }
    LOG(1, "[movie] open %s: %d track(s), %.1f s", guest_path, e->ntracks, e->fmt->duration > 0 ? e->fmt->duration / 1e6 : 0.0);
    return MOVIE_OK;
}
HLE(ex_set_source_path) { mtrace("ex_set_source_path", c); std::lock_guard<std::mutex> l(g_ex_lock); RET(ex_open(self<Extractor>(c), S(1))); }      // (path)
HLE(ex_set_source_reader) { mtrace("ex_set_source_reader", c); std::lock_guard<std::mutex> l(g_ex_lock); RET(ex_open(self<Extractor>(c), S(2))); }    // (ClientStreamReader*, path)
HLE(ex_set_source_mem) { mtrace("ex_set_source_mem", c); LOG(0, "[movie] SetDataSource(memory, %d bytes): not supported", (int)c->x[2]); RET(MOVIE_ERROR); }
HLE(ex_track_count) { mtrace("ex_track_count", c); std::lock_guard<std::mutex> l(g_ex_lock); Extractor *e = self<Extractor>(c); *(int32_t *)P(1) = e->fmt ? e->ntracks : 0; RET(MOVIE_OK); }
HLE(ex_track_config) { mtrace("ex_track_config", c);    // (int track, MediaData*)
    std::lock_guard<std::mutex> l(g_ex_lock);
    Extractor *e = self<Extractor>(c);
    int i = (int)c->x[1];
    MediaData *m = md(c->x[2]);
    m->e.clear(); m->par.reset();
    if (!e->fmt || i < 0 || i >= e->ntracks) { RET(MOVIE_ERROR); return; }
    AVStream *s = e->fmt->streams[e->map[i]];
    AVCodecParameters *cp = avcodec_parameters_alloc();
    avcodec_parameters_copy(cp, s->codecpar);
    m->par = std::shared_ptr<AVCodecParameters>(cp, [](AVCodecParameters *q) { avcodec_parameters_free(&q); });
    if (cp->codec_type == AVMEDIA_TYPE_VIDEO) {
        md_set_str(m, "mime", cp->codec_id == AV_CODEC_ID_VP9 ? "video/x-vnd.on2.vp9" : cp->codec_id == AV_CODEC_ID_VP8 ? "video/x-vnd.on2.vp8" : "video/avc");
        md_set_int(m, "width", cp->width);
        md_set_int(m, "height", cp->height);
        int fps = s->avg_frame_rate.den ? s->avg_frame_rate.num / s->avg_frame_rate.den : 30;
        md_set_int(m, "frame-rate", fps > 0 ? fps : 30);
    } else {
        md_set_str(m, "mime", cp->codec_id == AV_CODEC_ID_VORBIS ? "audio/vorbis" : cp->codec_id == AV_CODEC_ID_OPUS ? "audio/opus" : "audio/mp4a-latm");
        md_set_int(m, "sample-rate", cp->sample_rate);
        md_set_int(m, "channel-count", cp->ch_layout.nb_channels);
    }
    if (e->fmt->duration > 0) md_set_int(m, "durationUs", e->fmt->duration);
    RET(MOVIE_OK);
}
HLE(ex_select) { mtrace("ex_select", c); std::lock_guard<std::mutex> l(g_ex_lock); Extractor *e = self<Extractor>(c); int i = (int)c->x[1]; if (i >= 0 && i < e->ntracks) e->selected |= 1ull << e->map[i]; RET(MOVIE_OK); }
HLE(ex_unselect) { mtrace("ex_unselect", c); std::lock_guard<std::mutex> l(g_ex_lock); Extractor *e = self<Extractor>(c); int i = (int)c->x[1]; if (i >= 0 && i < e->ntracks) e->selected &= ~(1ull << e->map[i]); RET(MOVIE_OK); }
HLE(ex_seek) { mtrace("ex_seek", c);            // (int64 timeUs)
    std::lock_guard<std::mutex> l(g_ex_lock);
    Extractor *e = self<Extractor>(c);
    if (!e->fmt) { RET(MOVIE_ERROR); return; }
    if (e->have_pkt) { av_packet_unref(e->pkt); e->have_pkt = false; }
    e->eof = false;
    av_seek_frame(e->fmt, -1, (int64_t)c->x[1], AVSEEK_FLAG_BACKWARD);
    RET(MOVIE_OK);
}
HLE(ex_sample_track) { mtrace("ex_sample_track", c);    // GetTrackIndexForAvailableData(size_t*)
    std::lock_guard<std::mutex> l(g_ex_lock);
    Extractor *e = self<Extractor>(c);
    ex_fill(e);
    if (!e->have_pkt) { RET(MOVIE_ERROR); return; }
    uint64_t t = 0;
    for (int i = 0; i < e->ntracks; i++) if (e->map[i] == e->pkt->stream_index) t = (uint64_t)i;
    *(uint64_t *)P(1) = t;
    RET(MOVIE_OK);
}
HLE(ex_read) { mtrace("ex_read", c);            // (Buffer*)
    std::lock_guard<std::mutex> l(g_ex_lock);
    Extractor *e = self<Extractor>(c);
    MBuffer *b = BUF(c->x[1]);
    ex_fill(e);
    if (!e->have_pkt) { RET(MOVIE_ERROR); return; }
    uint32_t n = (uint32_t)e->pkt->size;
    if (n > b->capacity) { LOG(0, "[movie] packet of %u bytes > buffer of %llu", n, (unsigned long long)b->capacity); n = (uint32_t)b->capacity; }
    std::memcpy((void *)(uintptr_t)b->base, e->pkt->data, n);
    b->offset = 0; b->size = (int32_t)n;
    RET(MOVIE_OK);
}
HLE(ex_advance) { mtrace("ex_advance", c); std::lock_guard<std::mutex> l(g_ex_lock); Extractor *e = self<Extractor>(c); if (e->have_pkt) { av_packet_unref(e->pkt); e->have_pkt = false; } RET(MOVIE_OK); }
HLE(ex_sample_time) { mtrace("ex_sample_time", c);     // (int64* us)
    std::lock_guard<std::mutex> l(g_ex_lock);
    Extractor *e = self<Extractor>(c);
    ex_fill(e);
    if (!e->have_pkt) { RET(MOVIE_ERROR); return; }
    AVStream *s = e->fmt->streams[e->pkt->stream_index];
    int64_t ts = e->pkt->pts != AV_NOPTS_VALUE ? e->pkt->pts : e->pkt->dts;
    *(int64_t *)P(1) = av_rescale_q(ts, s->time_base, AVRational{1, 1000000});
    RET(MOVIE_OK);
}
HLE(ex_sample_meta) { mtrace("ex_sample_meta", c); MLOG("[movie] Extractor::GetSampleMetaData"); RET(MOVIE_ERROR); }
HLE(ex_buffered_range) { mtrace("ex_buffered_range", c);  // (int64* start, int64* end): a local file is all there
    std::lock_guard<std::mutex> l(g_ex_lock);
    Extractor *e = self<Extractor>(c);
    int64_t dur = e->fmt && e->fmt->duration > 0 ? e->fmt->duration : INT64_MAX / 4;
    if (c->x[1]) *(int64_t *)P(1) = 0;
    if (c->x[2]) *(int64_t *)P(2) = dur;
    RET(MOVIE_OK);
}
HLE(ex_dtor) { mtrace("ex_dtor", c);
    std::lock_guard<std::mutex> l(g_ex_lock);
    Extractor *e = self<Extractor>(c);
    if (!e) return;
    if (e->pkt) av_packet_free(&e->pkt);
    if (e->fmt) avformat_close_input(&e->fmt);
    delete e;
    ((uint64_t *)P(0))[1] = 0;
}
static const char *k_extractor_slots[] = {
    "movie:Extractor::D2", "movie:Extractor::D0", "movie:Extractor::SetDataSource(path)", "movie:Extractor::SetDataSource(mem)",
    "movie:Extractor::GetTrackCount", "movie:Extractor::GetTrackConfiguration", "movie:Extractor::SelectTrack",
    "movie:Extractor::UnselectTrack", "movie:Extractor::SeekTo", "movie:Extractor::GetTrackIndexForAvailableData",
    "movie:Extractor::Read", "movie:Extractor::Advance", "movie:Extractor::GetSampleTime", "movie:Extractor::GetSampleMetaData",
    "movie:Extractor::GetBufferedRange", "movie:Extractor::SetDataSource(reader)"};
static uint64_t g_extractor_vt, g_decoder_vt;
HLE(extractor_ctor) {     // (this, ContainerType)
    { std::lock_guard<std::mutex> l(g_ml); if (!g_extractor_vt) g_extractor_vt = make_vtable(k_extractor_slots, 16); }
    uint64_t *o = (uint64_t *)P(0);
    o[0] = g_extractor_vt; o[1] = (uint64_t)(uintptr_t)new Extractor();
    MLOG("[movie] Extractor created (container type %d)", (int)c->x[1]);
}

// ---------------------------------------------------------------- Decoder
enum { NIN = 4, NOUT = 6, NPENDING = 16 };
struct OutReady { int idx; int64_t time; uint32_t flags; };
struct Decoder {
    std::mutex m;
    AVCodecContext *ctx = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *pkt = nullptr;
    bool video = false;
    int out_format = 0, mode = 0;             // constructor arguments (DecoderOutputFormat, DecoderMode)
    uint64_t events[4] = {};                  // input available, output available, format changed, error
    uint8_t *in_mem[NIN] = {}; uint32_t in_cap = 0; bool in_busy[NIN] = {};
    uint8_t *out_mem[NOUT] = {}; uint32_t out_cap = 0, out_size[NOUT] = {}; bool out_busy[NOUT] = {};
    std::deque<OutReady> ready;
    int width = 0, height = 0, stride = 0, aligned_h = 0, sample_rate = 0, channels = 0;
    std::deque<std::pair<AVFrame *, int64_t>> pending;
    bool eos_pending = false, eos_in = false, stopped = true;
    int64_t eos_time = 0;
    uint64_t frames = 0;
};

// Guest-visible buffers come from plat_alloc (never freed): keep and reuse them across movies.
static std::mutex g_pool_lock;
static std::vector<std::pair<uint8_t *, uint32_t>> g_pool;
static uint8_t *buf_get(uint32_t size) {
    std::lock_guard<std::mutex> l(g_pool_lock);
    for (size_t i = 0; i < g_pool.size(); i++)
        if (g_pool[i].second == size) { uint8_t *p = g_pool[i].first; g_pool.erase(g_pool.begin() + (long)i); return p; }
    return (uint8_t *)plat_alloc(size);
}
static void buf_put(uint8_t *p, uint32_t size) { if (!p) return; std::lock_guard<std::mutex> l(g_pool_lock); g_pool.push_back({p, size}); }

static void dec_signal(Decoder *d, int i) {   // not after Stop (a stopped hardware decoder signals nothing)
    uint64_t ev;
    { std::lock_guard<std::mutex> l(d->m); if (d->stopped) return; ev = d->events[i]; }
    if (ev) xrt_os_signal_event(ev);
}

HLE(dec_configure) { mtrace("dec_configure", c);      // (MediaData*)
    Decoder *d = self<Decoder>(c);
    MediaData *m = md(c->x[1]);
    if (!m->par) { LOG(0, "[movie] Decoder::Configure without codec parameters"); RET(MOVIE_ERROR); return; }
    const AVCodecParameters *p = m->par.get();
    const AVCodec *codec = avcodec_find_decoder(p->codec_id);
    if (!codec) { LOG(0, "[movie] no decoder for codec %d", (int)p->codec_id); RET(MOVIE_ERROR); return; }
    std::lock_guard<std::mutex> l(d->m);
    d->ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(d->ctx, p);
    d->ctx->thread_count = 4;
    if (avcodec_open2(d->ctx, codec, nullptr) < 0) { LOG(0, "[movie] cannot open the %s decoder", codec->name); avcodec_free_context(&d->ctx); RET(MOVIE_ERROR); return; }
    d->frame = av_frame_alloc(); d->pkt = av_packet_alloc();
    d->video = p->codec_type == AVMEDIA_TYPE_VIDEO;
    d->width = p->width; d->height = p->height;
    d->stride = (p->width + 255) & ~255; d->aligned_h = (p->height + 15) & ~15;
    d->sample_rate = p->sample_rate; d->channels = p->ch_layout.nb_channels;
    d->in_cap = d->video ? 4u << 20 : 256u << 10;
    d->out_cap = d->video ? (uint32_t)(d->stride * d->aligned_h * 3 / 2) : 64u << 10;
    for (int i = 0; i < NIN; i++) d->in_mem[i] = buf_get(d->in_cap);
    for (int i = 0; i < NOUT; i++) d->out_mem[i] = buf_get(d->out_cap);
    LOG(1, "[movie] decoder %s: %dx%d %d Hz %d ch (output format %d, mode %d)", codec->name, d->width, d->height, d->sample_rate, d->channels, d->out_format, d->mode);
    RET(MOVIE_OK);
}
HLE(dec_register_events) { mtrace("dec_register_events", c); Decoder *d = self<Decoder>(c);
    if (movie_log()) {   // the DecoderEvents layout: dump it (and what its first words point at)
        const uint64_t *q = (const uint64_t *)P(1);
        for (int i = 0; i < 8; i++) LOG(0, "[movie]   events+%02x: %016llx", i * 8, (unsigned long long)q[i]);
        for (int i = 0; i < 4; i++) if (q[i] > 0x10000 && q[i] < (1ull << 48)) {
            const uint64_t *r = (const uint64_t *)(uintptr_t)q[i];
            LOG(0, "[movie]   *events[%d]: %016llx %016llx %016llx %016llx", i, (unsigned long long)r[0], (unsigned long long)r[1], (unsigned long long)r[2], (unsigned long long)r[3]);
        }
    }
    std::lock_guard<std::mutex> l(d->m); std::memcpy(d->events, P(1), sizeof d->events); RET(MOVIE_OK); }
static void dec_output_format(Decoder *d, MediaData *m) {
    m->e.clear(); m->par.reset();
    if (d->video) {
        md_set_str(m, "mime", "video/raw");
        md_set_int(m, "width", d->width); md_set_int(m, "height", d->height);
        md_set_int(m, "nv12-y-stride", d->stride); md_set_int(m, "nv12-y-offset", 0);
        md_set_int(m, "nv12-uv-offset", d->stride * d->aligned_h); md_set_int(m, "nv12-colorspace", 0);
    } else {
        md_set_str(m, "mime", "audio/raw");
        md_set_int(m, "sample-rate", d->sample_rate); md_set_int(m, "channel-count", d->channels);
    }
}
HLE(dec_get_output_format) { mtrace("dec_get_output_format", c); dec_output_format(self<Decoder>(c), md(c->x[1])); RET(MOVIE_OK); }
HLE(dec_get_output_buffer_format) { mtrace("dec_get_output_buffer_format", c); dec_output_format(self<Decoder>(c), md(c->x[2])); RET(MOVIE_OK); }
HLE(dec_start) { mtrace("dec_start", c);
    Decoder *d = self<Decoder>(c);
    { std::lock_guard<std::mutex> l(d->m); d->stopped = false; d->eos_in = false; }
    dec_signal(d, 0);         // every input buffer is free
    RET(MOVIE_OK);
}
// Stop: the game stops its decoders, deletes them, then tells its decoder thread to exit. An event of ours still
// set by then would wake that thread into a call through a deleted decoder: stop signalling and clear what is set.
HLE(dec_stop) { mtrace("dec_stop", c);
    Decoder *d = self<Decoder>(c);
    uint64_t ev[4];
    {
        std::lock_guard<std::mutex> l(d->m);
        if (d->stopped) { RET(MOVIE_OK); return; }
        d->stopped = true;
        std::memcpy(ev, d->events, sizeof ev);
    }
    for (int i = 0; i < 4; i++) if (ev[i]) xrt_os_clear_event(ev[i]);
    LOG(1, "[movie] %s decoder stopped after %llu frames", d->video ? "video" : "audio", (unsigned long long)d->frames);
    RET(MOVIE_OK);
}
HLE(dec_flush) { mtrace("dec_flush", c);
    Decoder *d = self<Decoder>(c);
    std::lock_guard<std::mutex> l(d->m);
    if (d->ctx) avcodec_flush_buffers(d->ctx);
    for (auto &p : d->pending) av_frame_free(&p.first);
    d->pending.clear(); d->ready.clear();
    for (int i = 0; i < NOUT; i++) d->out_busy[i] = false;
    for (int i = 0; i < NIN; i++) d->in_busy[i] = false;
    d->eos_pending = d->eos_in = false;
    RET(MOVIE_OK);
}
HLE(dec_acquire_input) { mtrace("dec_acquire_input", c);  // (int* index, int* remaining)
    Decoder *d = self<Decoder>(c);
    int idx = -1, free_n = 0;
    {
        std::lock_guard<std::mutex> l(d->m);
        int out_free = 0;
        for (int i = 0; i < NOUT; i++) if (!d->out_busy[i]) out_free++;
        if (out_free < 2 || !d->pending.empty()) { RET(MOVIE_ERROR); return; }   // backpressure: the game reads frames first
        for (int i = 0; i < NIN; i++) if (!d->in_busy[i]) { if (idx < 0) idx = i; else free_n++; }
        if (idx >= 0) d->in_busy[idx] = true;
    }
    if (idx < 0) { RET(MOVIE_ERROR); return; }
    *(int32_t *)P(1) = idx;
    if (c->x[2]) *(int32_t *)P(2) = free_n;
    RET(MOVIE_OK);
}
HLE(dec_get_input_buffer) { mtrace("dec_get_input_buffer", c);   // (int index, Buffer*)
    Decoder *d = self<Decoder>(c);
    int i = (int)c->x[1];
    if (i < 0 || i >= NIN) { RET(MOVIE_ERROR); return; }
    MBuffer *b = BUF(c->x[2]);
    std::memset(b, 0, sizeof *b);
    b->base = (uint64_t)(uintptr_t)d->in_mem[i]; b->capacity = d->in_cap;
    RET(MOVIE_OK);
}
// one decoded frame (or the end-of-stream marker) into a free output slot; d->m held
static bool emit_frame(Decoder *d, AVFrame *f, int64_t time, uint32_t flags) {
    int slot = -1;
    for (int i = 0; i < NOUT; i++) if (!d->out_busy[i]) { slot = i; break; }
    if (slot < 0) return false;
    uint8_t *o = d->out_mem[slot];
    uint32_t size = 0;
    if (flags & MOVIE_EOS) {
        size = 0;
    } else if (d->video) {    // planar 4:2:0 -> NV12 (Y plane, then interleaved UV) at the advertised stride
        int w = d->width, h = d->height, uv_off = d->stride * d->aligned_h;
        for (int y = 0; y < h; y++) std::memcpy(o + (size_t)y * d->stride, f->data[0] + (size_t)y * f->linesize[0], (size_t)w);
        for (int y = 0; y < h / 2; y++) {
            uint8_t *uv = o + uv_off + (size_t)y * d->stride;
            const uint8_t *u = f->data[1] + (size_t)y * f->linesize[1], *v = f->data[2] + (size_t)y * f->linesize[2];
            for (int x = 0; x < w / 2; x++) { uv[2 * x] = u[x]; uv[2 * x + 1] = v[x]; }
        }
        size = (uint32_t)(uv_off + d->stride * (h / 2));
    } else {                  // planar float -> interleaved s16 (Vorbis 5.1 kept in Vorbis order: the game reorders)
        int ch = f->ch_layout.nb_channels, n = f->nb_samples;
        int16_t *out = (int16_t *)o;
        if ((uint32_t)(n * ch * 2) > d->out_cap) n = (int)(d->out_cap / (uint32_t)(ch * 2));
        static const int vorbis51[6] = {0, 2, 1, 4, 5, 3};
        bool native = ch == 6 && d->ctx->codec_id == AV_CODEC_ID_VORBIS;
        for (int i = 0; i < n; i++)
            for (int k = 0; k < ch; k++) {
                int src = native ? vorbis51[k] : k;
                float s = f->format == AV_SAMPLE_FMT_FLTP ? ((const float *)f->data[src])[i] : 0.0f;
                s = s > 1.0f ? 1.0f : s < -1.0f ? -1.0f : s;
                out[i * ch + k] = (int16_t)(s * 32767.0f);
            }
        size = (uint32_t)(n * ch * 2);
    }
    d->out_busy[slot] = true;
    d->out_size[slot] = size;
    d->ready.push_back(OutReady{slot, time, flags});
    return true;
}
static bool flush_pending(Decoder *d) {   // d->m held
    bool any = false;
    while (!d->pending.empty() && emit_frame(d, d->pending.front().first, d->pending.front().second, 0)) {
        av_frame_free(&d->pending.front().first);
        d->pending.pop_front();
        any = true;
    }
    if (d->pending.empty() && d->eos_pending && emit_frame(d, nullptr, d->eos_time, MOVIE_EOS)) { d->eos_pending = false; any = true; }
    return any;
}
HLE(dec_send_input) { mtrace("dec_send_input", c);     // (int index, int offset, int size, int64 timeUs, uint32 flags)
    Decoder *d = self<Decoder>(c);
    int idx = (int)c->x[1], off = (int)c->x[2], size = (int)c->x[3];
    int64_t time = (int64_t)c->x[4];
    uint32_t flags = (uint32_t)c->x[5];
    bool produced = false;
    {
        std::lock_guard<std::mutex> l(d->m);
        if (!d->ctx) { RET(MOVIE_ERROR); return; }
        if (size > 0 && idx >= 0 && idx < NIN) {
            av_packet_unref(d->pkt);
            d->pkt->data = d->in_mem[idx] + off; d->pkt->size = size; d->pkt->pts = time;
            avcodec_send_packet(d->ctx, d->pkt);
            d->pkt->data = nullptr; d->pkt->size = 0;
        }
        if ((flags & MOVIE_EOS) && !d->eos_in) avcodec_send_packet(d->ctx, nullptr);
        while (avcodec_receive_frame(d->ctx, d->frame) == 0) {
            int64_t t = d->frame->pts != AV_NOPTS_VALUE ? d->frame->pts : time;
            if (!d->pending.empty() || !emit_frame(d, d->frame, t, 0)) {   // keep order behind what is pending
                if (d->pending.size() < NPENDING) d->pending.push_back({av_frame_clone(d->frame), t});
                else LOG(0, "[movie] output queue full, frame dropped");
            }
            d->frames++;
            produced = true;
        }
        // one end-of-stream output per stream (the game's reader repeats the flag on every buffer after the last
        // sample; each extra end output made the next movie show a stray frame in Monado's case)
        if ((flags & MOVIE_EOS) && !d->eos_in) { d->eos_in = true; d->eos_pending = true; d->eos_time = time; }
        produced |= flush_pending(d);
        if (idx >= 0 && idx < NIN) d->in_busy[idx] = false;
    }
    dec_signal(d, 0);
    if (produced) dec_signal(d, 1);
    RET(MOVIE_OK);
}
HLE(dec_acquire_output) { mtrace("dec_acquire_output", c); // (int* index, int64* timeUs, uint32* flags, int* remaining)
    Decoder *d = self<Decoder>(c);
    OutReady r; int remaining;
    {
        std::lock_guard<std::mutex> l(d->m);
        if (d->ready.empty()) { RET(MOVIE_ERROR); return; }
        r = d->ready.front(); d->ready.pop_front(); remaining = (int)d->ready.size();
    }
    *(int32_t *)P(1) = r.idx;
    if (c->x[2]) *(int64_t *)P(2) = r.time;
    if (c->x[3]) *(uint32_t *)P(3) = r.flags;
    if (c->x[4]) *(int32_t *)P(4) = remaining;
    RET(MOVIE_OK);
}
// GetOutputBuffer(index, Buffer*): a Buffer over the game's own memory gets a copy (the game then reads its memory,
// never the Buffer); otherwise the Buffer points at our slot.
static void fill_output(Decoder *d, int i, MBuffer *b) {
    bool ours = false;
    for (int k = 0; k < NOUT; k++)
        ours |= d->out_mem[k] && b->base >= (uint64_t)(uintptr_t)d->out_mem[k] && b->base < (uint64_t)(uintptr_t)d->out_mem[k] + d->out_cap;
    uint32_t size = d->out_size[i];
    if (b->base && !ours && b->capacity >= size) {
        std::memcpy((void *)(uintptr_t)b->base, d->out_mem[i], size);
        b->offset = 0; b->size = (int32_t)size;
        return;
    }
    std::memset(b, 0, sizeof *b);
    b->base = (uint64_t)(uintptr_t)d->out_mem[i]; b->capacity = d->out_cap; b->size = (int32_t)size;
}
HLE(dec_get_output_buffer) { mtrace("dec_get_output_buffer", c);
    Decoder *d = self<Decoder>(c);
    int i = (int)c->x[1];
    MBuffer *b = BUF(c->x[2]);
    { static int n; if (movie_log() && n++ < 8) LOG(0, "[movie] GetOutputBuffer(%s) slot %d: game buffer base %llx capacity %llu (lr %llx)", d->video ? "video" : "audio", i, (unsigned long long)b->base, (unsigned long long)b->capacity, (unsigned long long)c->x[30]); }
    if (i < 0 || i >= NOUT) { RET(MOVIE_ERROR); return; }
    fill_output(d, i, b);
    RET(MOVIE_OK);
}
HLE(dec_get_output_buffer2) { mtrace("dec_get_output_buffer2", c);   // (index, Buffer*, MediaData* format)
    Decoder *d = self<Decoder>(c);
    int i = (int)c->x[1];
    if (i < 0 || i >= NOUT) { RET(MOVIE_ERROR); return; }
    fill_output(d, i, BUF(c->x[2]));
    if (c->x[3]) dec_output_format(d, md(c->x[3]));
    MLOG("[movie] GetOutputBuffer(index, Buffer, MediaData) slot %d", i);
    RET(MOVIE_OK);
}
HLE(dec_get_output_buffer_size) { mtrace("dec_get_output_buffer_size", c);   // (int*, int*) - meaning unconfirmed: log what the game does with it
    Decoder *d = self<Decoder>(c);
    if (c->x[1]) *(int32_t *)P(1) = (int32_t)d->out_cap;
    if (c->x[2]) *(int32_t *)P(2) = NOUT;
    MLOG("[movie] GetOutputBufferSize -> %u, %d (lr %llx)", d->out_cap, NOUT, (unsigned long long)c->x[30]);
    RET(MOVIE_OK);
}
HLE(dec_release_output) { mtrace("dec_release_output", c);     // (int index)
    Decoder *d = self<Decoder>(c);
    dec_signal(d, 0);
    bool produced;
    {
        std::lock_guard<std::mutex> l(d->m);
        int i = (int)c->x[1];
        if (i >= 0 && i < NOUT) d->out_busy[i] = false;
        produced = flush_pending(d);
    }
    if (produced) dec_signal(d, 1);
    RET(MOVIE_OK);
}
HLE(dec_last_error) { mtrace("dec_last_error", c); RET(MOVIE_OK); }
HLE(dec_dtor) { mtrace("dec_dtor", c);
    Decoder *d = self<Decoder>(c);
    if (!d) return;
    dec_stop(c);
    {
        std::lock_guard<std::mutex> l(d->m);
        if (d->ctx) avcodec_free_context(&d->ctx);
        for (int i = 0; i < NIN; i++) { buf_put(d->in_mem[i], d->in_cap); d->in_mem[i] = nullptr; }
        for (int i = 0; i < NOUT; i++) { buf_put(d->out_mem[i], d->out_cap); d->out_mem[i] = nullptr; }
        for (auto &p : d->pending) av_frame_free(&p.first);
        d->pending.clear(); d->ready.clear();
        av_frame_free(&d->frame); av_packet_free(&d->pkt);
    }
    // the Decoder itself is kept: a game thread may still hold the object for one more call
    RET(0);
}
static const char *k_decoder_slots[] = {
    "movie:Decoder::D2", "movie:Decoder::D0", "movie:Decoder::Configure", "movie:Decoder::RegisterClientEvents",
    "movie:Decoder::GetOutputFormat", "movie:Decoder::Start", "movie:Decoder::Stop", "movie:Decoder::Reset", "movie:Decoder::Flush",
    "movie:Decoder::AcquireInputBufferIndex", "movie:Decoder::GetInputBuffer", "movie:Decoder::SendInputBufferForDecode",
    "movie:Decoder::AcquireOutputBufferIndex", "movie:Decoder::GetOutputBuffer", "movie:Decoder::GetOutputBufferSize",
    "movie:Decoder::ReleaseOutputBufferIndex", "movie:Decoder::GetLastError", "movie:Decoder::GetOutputBuffer2",
    "movie:Decoder::GetOutputBufferFormat"};
HLE(decoder_ctor) {       // (this, DecoderType, DecoderOutputFormat, DecoderMode, int)
    { std::lock_guard<std::mutex> l(g_ml); if (!g_decoder_vt) g_decoder_vt = make_vtable(k_decoder_slots, 19); }
    Decoder *d = new Decoder();
    d->out_format = (int)c->x[2]; d->mode = (int)c->x[3];
    uint64_t *o = (uint64_t *)P(0);
    o[0] = g_decoder_vt; o[1] = (uint64_t)(uintptr_t)d;
    LOG(1, "[movie] Decoder created (type %d, output format %d, mode %d, %d)", (int)c->x[1], (int)c->x[2], (int)c->x[3], (int)c->x[4]);
}

#define MOVIE_ENTRIES \
    E("_ZN5movie9ExtractorC1ENS_13ContainerTypeE", extractor_ctor), \
    E("_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", decoder_ctor), \
    E("movie:Extractor::D2", ex_dtor), E("movie:Extractor::D0", ex_dtor), \
    E("movie:Extractor::SetDataSource(path)", ex_set_source_path), E("movie:Extractor::SetDataSource(mem)", ex_set_source_mem), \
    E("movie:Extractor::SetDataSource(reader)", ex_set_source_reader), E("movie:Extractor::GetTrackCount", ex_track_count), \
    E("movie:Extractor::GetTrackConfiguration", ex_track_config), E("movie:Extractor::SelectTrack", ex_select), \
    E("movie:Extractor::UnselectTrack", ex_unselect), E("movie:Extractor::SeekTo", ex_seek), \
    E("movie:Extractor::GetTrackIndexForAvailableData", ex_sample_track), E("movie:Extractor::Read", ex_read), \
    E("movie:Extractor::Advance", ex_advance), E("movie:Extractor::GetSampleTime", ex_sample_time), \
    E("movie:Extractor::GetSampleMetaData", ex_sample_meta), E("movie:Extractor::GetBufferedRange", ex_buffered_range), \
    E("movie:Decoder::D2", dec_dtor), E("movie:Decoder::D0", dec_dtor), \
    E("movie:Decoder::Configure", dec_configure), E("movie:Decoder::RegisterClientEvents", dec_register_events), \
    E("movie:Decoder::GetOutputFormat", dec_get_output_format), E("movie:Decoder::Start", dec_start), E("movie:Decoder::Stop", dec_stop), \
    E("movie:Decoder::Reset", dec_flush), E("movie:Decoder::Flush", dec_flush), E("movie:Decoder::AcquireInputBufferIndex", dec_acquire_input), \
    E("movie:Decoder::GetInputBuffer", dec_get_input_buffer), E("movie:Decoder::SendInputBufferForDecode", dec_send_input), \
    E("movie:Decoder::AcquireOutputBufferIndex", dec_acquire_output), E("movie:Decoder::GetOutputBuffer", dec_get_output_buffer), \
    E("movie:Decoder::GetOutputBufferSize", dec_get_output_buffer_size), E("movie:Decoder::ReleaseOutputBufferIndex", dec_release_output), \
    E("movie:Decoder::GetLastError", dec_last_error), E("movie:Decoder::GetOutputBuffer2", dec_get_output_buffer2), \
    E("movie:Decoder::GetOutputBufferFormat", dec_get_output_buffer_format), \
    E("_ZN5movie9MediaDataC1Ev", md_ctor), E("_ZN5movie9MediaDataC1ERKS0_", md_copy), E("_ZN5movie9MediaDataaSERKS0_", md_assign), \
    E("_ZN5movie9MediaDataD1Ev", md_dtor), E("_ZN5movie9MediaData5ClearEv", md_clear), E("_ZN5movie9MediaData8SetInt32EPKci", md_set_int32), \
    E("_ZNK5movie9MediaData9FindInt32EPKcPi", md_find_int32), E("_ZNK5movie9MediaData10FindStringEPKcPS2_", md_find_string), \
    E("_ZN5movie6BufferC1Ev", buf_ctor), E("_ZN5movie6BufferC1EPvm", buf_ctor_ext), E("_ZN5movie6BufferC1Em", buf_ctor_alloc), \
    E("_ZN5movie6BufferC1ERKS0_", buf_copy), E("_ZN5movie6BufferaSERKS0_", buf_copy), E("_ZN5movie6BufferD1Ev", buf_dtor), \
    E("_ZNK5movie6Buffer4BaseEv", buf_base), E("_ZNK5movie6Buffer4SizeEv", buf_size), E("_ZNK5movie6Buffer6OffsetEv", buf_offset), \
    E("_ZNK5movie6Buffer8CapacityEv", buf_capacity), E("_ZN5movie6Buffer8SetRangeEii", buf_set_range), \
    E("_ZN5movie6Buffer18SetDataAndCapacityEPvm", buf_set_data), E("_ZN5movie6Buffer12SetInt64DataEl", buf_set_i64), \
    E("_ZNK5movie6Buffer12GetInt64DataEv", buf_get_i64), E("_ZN5movie6Buffer13SetUint32DataEj", buf_set_u32), \
    E("_ZNK5movie6Buffer13GetUint32DataEv", buf_get_u32), E("_ZN5movie6Buffer12SetInt32DataEi", buf_set_i32), \
    E("_ZN5movie11SetCoreMaskEm", m_ok), E("_ZN5movie12SetAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", m_ok), \
    E("_ZN5movie17GetThreadPriorityEv", m_prio),

// ---------------------------------------------------------------- nn::os message queues
struct MQ { std::mutex m; std::condition_variable cv; std::deque<uint64_t> q; size_t cap = 1; };
static std::unordered_map<uint64_t, MQ *> g_mqs;
static MQ *mq(uint64_t a) { std::lock_guard<std::mutex> l(g_ml); MQ *&q = g_mqs[a]; if (!q) q = new MQ; return q; }
HLE(mq_init) { MQ *q = mq(c->x[0]); std::lock_guard<std::mutex> l(q->m); q->q.clear(); q->cap = c->x[2] ? c->x[2] : 1; }
HLE(mq_fini) { }
HLE(mq_send) {       // (mq, value) - blocks while full
    MQ *q = mq(c->x[0]); std::unique_lock<std::mutex> l(q->m);
    q->cv.wait(l, [q] { return q->q.size() < q->cap; }); q->q.push_back(c->x[1]); q->cv.notify_all();
}
HLE(mq_try_send) {
    MQ *q = mq(c->x[0]); std::lock_guard<std::mutex> l(q->m);
    if (q->q.size() >= q->cap) { RET(0); return; }
    q->q.push_back(c->x[1]); q->cv.notify_all(); RET(1);
}
HLE(mq_jam) {        // send to the front
    MQ *q = mq(c->x[0]); std::unique_lock<std::mutex> l(q->m);
    q->cv.wait(l, [q] { return q->q.size() < q->cap; }); q->q.push_front(c->x[1]); q->cv.notify_all();
}
HLE(mq_receive) {    // (u64* out, mq) - blocks while empty
    MQ *q = mq(c->x[1]); std::unique_lock<std::mutex> l(q->m);
    q->cv.wait(l, [q] { return !q->q.empty(); });
    *(uint64_t *)P(0) = q->q.front(); q->q.pop_front(); q->cv.notify_all();
}
HLE(mq_try_receive) {
    MQ *q = mq(c->x[1]); std::lock_guard<std::mutex> l(q->m);
    if (q->q.empty()) { RET(0); return; }
    *(uint64_t *)P(0) = q->q.front(); q->q.pop_front(); q->cv.notify_all(); RET(1);
}
HLE(mq_timed_receive) {   // (u64* out, mq, TimeSpan ns)
    MQ *q = mq(c->x[1]); std::unique_lock<std::mutex> l(q->m);
    if (!q->cv.wait_for(l, std::chrono::nanoseconds((int64_t)c->x[2]), [q] { return !q->q.empty(); })) { RET(0); return; }
    *(uint64_t *)P(0) = q->q.front(); q->q.pop_front(); q->cv.notify_all(); RET(1);
}
HLE(mq_try_peek) {
    MQ *q = mq(c->x[1]); std::lock_guard<std::mutex> l(q->m);
    if (q->q.empty()) { RET(0); return; }
    *(uint64_t *)P(0) = q->q.front(); RET(1);
}

#define E(n, f) {n, f}
extern "C" const HleImpl hle_movie[] = {
    MOVIE_ENTRIES
    E("_ZN2nn2os22InitializeMessageQueueEPNS0_16MessageQueueTypeEPmm", mq_init),
    E("_ZN2nn2os20FinalizeMessageQueueEPNS0_16MessageQueueTypeE", mq_fini),
    E("_ZN2nn2os16SendMessageQueueEPNS0_16MessageQueueTypeEm", mq_send),
    E("_ZN2nn2os19TrySendMessageQueueEPNS0_16MessageQueueTypeEm", mq_try_send),
    E("_ZN2nn2os15JamMessageQueueEPNS0_16MessageQueueTypeEm", mq_jam),
    E("_ZN2nn2os19ReceiveMessageQueueEPmPNS0_16MessageQueueTypeE", mq_receive),
    E("_ZN2nn2os22TryReceiveMessageQueueEPmPNS0_16MessageQueueTypeE", mq_try_receive),
    E("_ZN2nn2os24TimedReceiveMessageQueueEPmPNS0_16MessageQueueTypeENS_8TimeSpanE", mq_timed_receive),
    E("_ZN2nn2os19TryPeekMessageQueueEPmPKNS0_16MessageQueueTypeE", mq_try_peek),
    {nullptr, nullptr}
};
