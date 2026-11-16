// nn::audio (AudioOut, device info, resampler) and nn::codec::HardwareOpusDecoder.
//
//  - AudioOut: the game (Wwise) mixes everything itself and appends 16-bit PCM buffers. With a host device
//    (audio_host.cpp) a buffer is copied to the output ring and released while the ring holds less than the
//    target latency: the sound card's clock paces the game's audio thread, as the Switch's does. Without one
//    (XRT_NO_AUDIO=1, no device) buffers are consumed at wall-clock pace, silently.
//  - HardwareOpusDecoder: the Switch packet framing (8-byte header: big-endian size, final range) around
//    plain Opus packets, decoded with libopus (third_party/opus). XRT_NO_OPUS=1: silence of the right length.
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <opus.h>
#include "audio_host.h"
extern "C" {
#include "xrt_internal.h"
}
#define P(i) ((void *)(uintptr_t)c->x[i])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define OK() RET(0)
#define HLE(name) static void name(Ctx *c)

static const int OUT_RATE = 48000, OUT_CHANNELS = 2;

// ---------------------------------------------------------------- device info
HLE(dev_channel_count) { RET(OUT_CHANNELS); }
HLE(dev_name) { std::strcpy((char *)P(0), "AudioTvOutput"); }   // AudioDeviceName = char[256]
HLE(dev_notification) { OK(); }

// ---------------------------------------------------------------- AudioOut
// Guest AudioOutBuffer layout is private to these functions: +0x00 next (ours), +0x08 data,
// +0x10 capacity, +0x18 size.
struct Out { int rate = OUT_RATE, ch = OUT_CHANNELS; bool started = false, host = false;
             std::deque<uint64_t> queue, released; double play_pos_ns = 0; uint64_t last_ns = 0; };
static std::mutex g_alock;
static std::unordered_map<uint64_t, Out> g_outs;

HLE(ao_init_param) { int32_t *p = (int32_t *)P(0); p[0] = OUT_RATE; ((uint16_t *)p)[2] = OUT_CHANNELS; ((uint16_t *)p)[3] = 0; }
HLE(ao_open_default) {   // (AudioOut*, const AudioOutParameter&)
    std::lock_guard<std::mutex> l(g_alock);
    Out &o = g_outs[c->x[0]];
    int32_t rate = *(int32_t *)P(1);
    uint16_t ch = ((uint16_t *)P(1))[2];
    o.rate = rate ? rate : OUT_RATE;
    o.ch = ch ? ch : OUT_CHANNELS;
    LOG(1, "[audio] AudioOut opened: %d Hz, %d ch", o.rate, o.ch);
    OK();
}
HLE(ao_start) {
    std::lock_guard<std::mutex> l(g_alock); Out &o = g_outs[c->x[0]]; o.started = true; o.last_ns = plat_ticks_ns();
    // one host stream: the first AudioOut the game starts (Wwise opens exactly one)
    static uint64_t host_out = 0;
    if (!host_out && audio_host_open(o.rate, o.ch)) host_out = c->x[0];
    o.host = host_out == c->x[0];
    OK();
}
HLE(ao_stop) { std::lock_guard<std::mutex> l(g_alock); g_outs[c->x[0]].started = false; OK(); }
HLE(ao_close) { std::lock_guard<std::mutex> l(g_alock); g_outs.erase(c->x[0]); }
HLE(ao_state) { std::lock_guard<std::mutex> l(g_alock); RET(g_outs[c->x[0]].started ? 0 : 1); }   // 0 Started, 1 Stopped
HLE(ao_rate) { std::lock_guard<std::mutex> l(g_alock); RET(g_outs[c->x[0]].rate); }
HLE(ao_channels) { std::lock_guard<std::mutex> l(g_alock); RET(g_outs[c->x[0]].ch); }
HLE(ao_format) { RET(2); }   // SampleFormat_PcmInt16
HLE(sample_byte_size) { uint32_t f = (uint32_t)c->x[0]; RET(f == 1 ? 1 : f == 2 ? 2 : f == 3 ? 3 : 4); }
HLE(ao_set_buffer_info) {   // (AudioOutBuffer*, void* data, size_t capacity, size_t size)
    uint64_t *b = (uint64_t *)P(0);
    b[0] = 0; b[1] = c->x[1]; b[2] = c->x[2]; b[3] = c->x[3];
}
HLE(ao_buffer_ptr) { RET(((uint64_t *)P(0))[1]); }
HLE(ao_buffer_size) { RET(((uint64_t *)P(0))[3]); }

static void ao_advance(Out &o) {
    if (o.host) {   // the device's clock: move buffers to the ring while it is below the target latency
        if (!o.started) return;
        static uint64_t next_stats; uint64_t now = plat_ticks_ns();
        if (now >= next_stats) { if (next_stats) audio_host_stats(); next_stats = now + 30000000000ull; }
        while (!o.queue.empty() && audio_host_queued_frames() < audio_host_target_frames()) {
            uint64_t b = o.queue.front();
            const uint64_t *bi = (const uint64_t *)(uintptr_t)b;
            audio_host_write((const int16_t *)(uintptr_t)bi[1], bi[3] / (2 * (size_t)o.ch));
            o.queue.pop_front();
            o.released.push_back(b);
        }
        return;
    }
    uint64_t now = plat_ticks_ns();
    if (!o.started) { o.last_ns = now; return; }
    o.play_pos_ns += (double)(now - o.last_ns);
    o.last_ns = now;
    while (!o.queue.empty()) {
        uint64_t b = o.queue.front();
        uint64_t size = ((uint64_t *)(uintptr_t)b)[3];
        double dur = (double)size / (2.0 * o.ch) / o.rate * 1e9;
        if (o.play_pos_ns < dur) break;
        o.play_pos_ns -= dur;
        o.queue.pop_front();
        o.released.push_back(b);
    }
    if (o.queue.empty()) o.play_pos_ns = 0;     // underrun: don't bank time
}
HLE(ao_append) { std::lock_guard<std::mutex> l(g_alock); Out &o = g_outs[c->x[0]]; ao_advance(o); o.queue.push_back(c->x[1]); RET(1); }
HLE(ao_get_released) {
    std::lock_guard<std::mutex> l(g_alock);
    Out &o = g_outs[c->x[0]];
    ao_advance(o);
    if (o.released.empty()) { RET(0); return; }
    uint64_t b = o.released.front(); o.released.pop_front(); RET(b);
}

// ---------------------------------------------------------------- resampler (linear)
// ResamplerType is opaque guest storage; we keep the state on the host keyed by its address.
struct Rs { int ch, in_rate, out_rate; double pos = 0; int16_t last[8] = {}; };
static std::unordered_map<uint64_t, Rs> g_rs;
HLE(rs_required_size) { RET(0x100); }
HLE(rs_init) {   // (ResamplerType*, void* buf, size_t size, int inRate, int outRate, int channels)
    std::lock_guard<std::mutex> l(g_alock);
    g_rs[c->x[0]] = Rs{(int)c->x[5], (int)c->x[3], (int)c->x[4]};
    OK();
}
HLE(rs_output_count) {   // (const ResamplerType*, int inputSamples)
    std::lock_guard<std::mutex> l(g_alock);
    Rs &r = g_rs[c->x[0]];
    RET((int64_t)((int64_t)(int32_t)c->x[1] * r.out_rate / (r.in_rate ? r.in_rate : 1) + 1));
}
HLE(rs_process) {   // (ResamplerType*, int* outCount, int16* out, size_t outCapacity, const int16* in, int inCount)
    std::lock_guard<std::mutex> l(g_alock);
    Rs &r = g_rs[c->x[0]];
    int32_t *out_count = (int32_t *)P(1);
    int16_t *out = (int16_t *)P(2);
    size_t cap = c->x[3] / sizeof(int16_t);
    const int16_t *in = (const int16_t *)P(4);
    int n = (int)c->x[5], ch = r.ch > 0 ? r.ch : 1;
    double step = (double)r.in_rate / (r.out_rate ? r.out_rate : 1);
    int produced = 0;
    while (r.pos < n && (size_t)(produced + 1) * ch <= cap) {
        int i = (int)r.pos; double fr = r.pos - i;
        for (int k = 0; k < ch; k++) {
            int16_t a = i > 0 ? in[(i - 1) * ch + k] : r.last[k], b = in[i * ch + k];
            out[produced * ch + k] = (int16_t)(a + (b - a) * fr);
        }
        produced++; r.pos += step;
    }
    r.pos -= n;
    if (n > 0) for (int k = 0; k < ch && k < 8; k++) r.last[k] = in[(n - 1) * ch + k];
    *out_count = produced;
    OK();
}

// ---------------------------------------------------------------- HardwareOpusDecoder
// Real vtable (from sdk.elf): D1, D0, GetWorkBufferSize, IsInitialized, Initialize, Finalize,
// GetSampleRate, GetChannelCount, Reset, DecodeInterleaved. The object's first word points at it.
struct Opus { int rate = 48000, ch = 1; bool init = false; OpusDecoder *dec = nullptr; };
static std::unordered_map<uint64_t, Opus> g_opus;
static uint64_t g_opus_vtbl;

static int opus_packet_samples(const uint8_t *pkt, size_t len, int rate) {
    if (!len) return 0;
    int config = pkt[0] >> 3, code = pkt[0] & 3;
    static const int us[32] = {10000, 20000, 40000, 60000, 10000, 20000, 40000, 60000, 10000, 20000, 40000, 60000,
                               10000, 20000, 10000, 20000, 2500, 5000, 10000, 20000, 2500, 5000, 10000, 20000,
                               2500, 5000, 10000, 20000, 2500, 5000, 10000, 20000};
    int frames = code == 0 ? 1 : code == 3 ? (len > 1 ? (pkt[1] & 0x3f) : 1) : 2;
    return (int)((int64_t)us[config] * frames * rate / 1000000);
}
static void opus_free(uint64_t obj) {   // (g_alock held)
    auto it = g_opus.find(obj);
    if (it == g_opus.end()) return;
    if (it->second.dec) opus_decoder_destroy(it->second.dec);
    g_opus.erase(it);
}
HLE(opus_dtor) { std::lock_guard<std::mutex> l(g_alock); opus_free(c->x[0]); }
HLE(opus_dtor_delete) {
    { std::lock_guard<std::mutex> l(g_alock); opus_free(c->x[0]); }
    static uint64_t del = xrt_find_export("_ZdlPv");
    xrt_call(c, del, 1, c->x[0]);
}
HLE(opus_work_size) { RET(0x4000); }
HLE(opus_is_init) { std::lock_guard<std::mutex> l(g_alock); RET(g_opus[c->x[0]].init ? 1 : 0); }
HLE(opus_initialize) {   // (this, sampleRate, channelCount, void* work, size_t workSize)
    std::lock_guard<std::mutex> l(g_alock);
    Opus &o = g_opus[c->x[0]];
    o.rate = (int)c->x[1]; o.ch = (int)c->x[2]; o.init = true;
    if (o.dec) { opus_decoder_destroy(o.dec); o.dec = nullptr; }
    static int off = -1; if (off < 0) off = getenv("XRT_NO_OPUS") ? 1 : 0;
    int err = OPUS_OK;
    if (!off) o.dec = opus_decoder_create(o.rate, o.ch, &err);
    static int n; if (n++ < 4 || (!off && !o.dec)) LOG(1, "[audio] Opus decoder %d Hz %d ch%s", o.rate, o.ch, off ? " (silent: XRT_NO_OPUS)" : o.dec ? "" : " - libopus refused it, silent");
    OK();
}
HLE(opus_finalize) {
    std::lock_guard<std::mutex> l(g_alock); Opus &o = g_opus[c->x[0]]; o.init = false;
    if (o.dec) { opus_decoder_destroy(o.dec); o.dec = nullptr; }
}
HLE(opus_rate) { std::lock_guard<std::mutex> l(g_alock); RET(g_opus[c->x[0]].rate); }
HLE(opus_channels) { std::lock_guard<std::mutex> l(g_alock); RET(g_opus[c->x[0]].ch); }
HLE(opus_reset) { std::lock_guard<std::mutex> l(g_alock); Opus &o = g_opus[c->x[0]]; if (o.dec) opus_decoder_ctl(o.dec, OPUS_RESET_STATE); OK(); }
HLE(opus_decode) {   // (this, size_t* consumed, int* sampleCount, int16* out, size_t outSize, const void* in, size_t inSize)
    Opus o;
    { std::lock_guard<std::mutex> l(g_alock); o = g_opus[c->x[0]]; }
    const uint8_t *in = (const uint8_t *)P(5);
    size_t in_size = c->x[6];
    if (in_size < 8) { RET((2 << 9) | 111); return; }               // codec: invalid packet
    uint32_t len = ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) | ((uint32_t)in[2] << 8) | in[3];
    if (8 + (size_t)len > in_size) { RET((2 << 9) | 111); return; }
    int samples = opus_packet_samples(in + 8, len, o.rate);
    size_t cap = c->x[4] / (sizeof(int16_t) * (o.ch ? o.ch : 1));
    if ((size_t)samples > cap) samples = (int)cap;
    int16_t *out = (int16_t *)P(3);
    // a decoder object is used by one guest thread at a time (Wwise's decoding thread), so libopus runs unlocked
    int got = o.dec ? opus_decode(o.dec, in + 8, (opus_int32)len, out, (int)cap, 0) : -1;
    if (got >= 0) samples = got;
    else {
        if (o.dec) { static int n; if (n++ < 8) LOG(1, "[audio] opus_decode failed (%d) on a %u-byte packet", got, len); }
        std::memset(out, 0, (size_t)samples * o.ch * sizeof(int16_t));
    }
    *(uint64_t *)P(1) = 8 + len;
    *(int32_t *)P(2) = samples;
    OK();
}
HLE(opus_ctor) {   // HardwareOpusDecoder(int option)
    {
        std::lock_guard<std::mutex> l(g_alock);
        if (!g_opus_vtbl) {
            static const char *slots[] = {"opus:D1", "opus:D0", "opus:GetWorkBufferSize", "opus:IsInitialized", "opus:Initialize",
                                          "opus:Finalize", "opus:GetSampleRate", "opus:GetChannelCount", "opus:Reset", "opus:DecodeInterleaved"};
            uint64_t *vt = (uint64_t *)plat_alloc(sizeof(uint64_t) * 12);
            for (int i = 0; i < 10; i++) vt[2 + i] = hle_resolve(slots[i]);
            g_opus_vtbl = (uint64_t)(uintptr_t)(vt + 2);
        }
        g_opus[c->x[0]] = Opus{};
    }
    *(uint64_t *)P(0) = g_opus_vtbl;
}

extern "C" const HleImpl hle_audio[] = {
    {"_ZN2nn5audio41GetActiveAudioDeviceChannelCountForOutputEv", dev_channel_count},
    {"_ZN2nn5audio24GetActiveAudioDeviceNameEPNS0_15AudioDeviceNameE", dev_name},
    {"_ZN2nn5audio39AcquireAudioDeviceNotificationForOutputEPNS_2os11SystemEventE", dev_notification},
    {"_ZN2nn5audio27InitializeAudioOutParameterEPNS0_17AudioOutParameterE", ao_init_param},
    {"_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutERKNS0_17AudioOutParameterE", ao_open_default},
    {"_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE", ao_start},
    {"_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE", ao_stop},
    {"_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE", ao_close},
    {"_ZN2nn5audio16GetAudioOutStateEPKNS0_8AudioOutE", ao_state},
    {"_ZN2nn5audio21GetAudioOutSampleRateEPKNS0_8AudioOutE", ao_rate},
    {"_ZN2nn5audio23GetAudioOutChannelCountEPKNS0_8AudioOutE", ao_channels},
    {"_ZN2nn5audio23GetAudioOutSampleFormatEPKNS0_8AudioOutE", ao_format},
    {"_ZN2nn5audio17GetSampleByteSizeENS0_12SampleFormatE", sample_byte_size},
    {"_ZN2nn5audio21SetAudioOutBufferInfoEPNS0_14AudioOutBufferEPvmm", ao_set_buffer_info},
    {"_ZN2nn5audio28GetAudioOutBufferDataPointerEPKNS0_14AudioOutBufferE", ao_buffer_ptr},
    {"_ZN2nn5audio25GetAudioOutBufferDataSizeEPKNS0_14AudioOutBufferE", ao_buffer_size},
    {"_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE", ao_append},
    {"_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE", ao_get_released},
    {"_ZN2nn5audio33GetRequiredBufferSizeForResamplerEi", rs_required_size},
    {"_ZN2nn5audio19InitializeResamplerEPNS0_13ResamplerTypeEPvmiii", rs_init},
    {"_ZN2nn5audio29GetResamplerOutputSampleCountEPKNS0_13ResamplerTypeEi", rs_output_count},
    {"_ZN2nn5audio22ProcessResamplerBufferEPNS0_13ResamplerTypeEPiPsmPKsi", rs_process},
    {"_ZN2nn5codec19HardwareOpusDecoderC1Ei", opus_ctor},
    {"opus:D1", opus_dtor}, {"opus:D0", opus_dtor_delete}, {"opus:GetWorkBufferSize", opus_work_size},
    {"opus:IsInitialized", opus_is_init}, {"opus:Initialize", opus_initialize}, {"opus:Finalize", opus_finalize},
    {"opus:GetSampleRate", opus_rate}, {"opus:GetChannelCount", opus_channels}, {"opus:Reset", opus_reset},
    {"opus:DecodeInterleaved", opus_decode},
    {nullptr, nullptr}
};
