// Host audio output (milestone M4): the game's AudioOut PCM goes to the default playback device through
// miniaudio (third_party/miniaudio, public domain / MIT-0): WASAPI on Windows, PulseAudio/ALSA on Linux.
//
// The game's audio thread (Wwise) appends 16-bit PCM buffers and polls for released ones, like on the Switch,
// where the audio hardware's clock decides when a buffer is done. Here the sound card's clock does: hle_audio
// copies a buffer into a lock-free ring and releases it only while the ring holds less than the target latency,
// and the device callback drains the ring. If no device can be opened, hle_audio falls back to its wall-clock model.
//
// XRT_NO_AUDIO=1: no host output (silent, wall-clock timing). XRT_AUDIO_LATENCY_MS=n: ring target (default 60).
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#if defined(__clang__)
#pragma clang diagnostic ignored "-Weverything"
#endif
#include "miniaudio/miniaudio.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
extern "C" {
#include "xrt_internal.h"
}
#include "audio_host.h"
#include "settings.h"

static std::mutex g_lock;              // open / close only
static bool g_tried, g_ok;
static ma_device g_dev;
static ma_pcm_rb g_rb;
static int g_rate, g_ch;
static std::atomic<uint64_t> g_underruns{0}, g_played{0};
static std::atomic<bool> g_running{false};

static void callback(ma_device *, void *out, const void *, ma_uint32 frames) {
    int16_t *dst = (int16_t *)out;
    ma_uint32 done = 0;
    while (done < frames) {
        ma_uint32 n = frames - done;
        void *src = nullptr;
        if (ma_pcm_rb_acquire_read(&g_rb, &n, &src) != MA_SUCCESS || n == 0) break;
        std::memcpy(dst + (size_t)done * g_ch, src, (size_t)n * g_ch * sizeof(int16_t));
        ma_pcm_rb_commit_read(&g_rb, n);
        done += n;
    }
    if (done < frames) {
        std::memset(dst + (size_t)done * g_ch, 0, (size_t)(frames - done) * g_ch * sizeof(int16_t));
        if (g_running.load(std::memory_order_relaxed) && done > 0) g_underruns++;
    }
    g_played += done;
    // volume (xc3.ini / TAB menu): a cubic curve, so the slider feels even
    int vol = settings().volume;
    if (vol < 100) {
        float v = vol / 100.0f; v = v * v * v;
        size_t ns = (size_t)done * g_ch;
        for (size_t i = 0; i < ns; i++) dst[i] = (int16_t)(dst[i] * v);
    }
}

bool audio_host_open(int rate, int channels) {
    std::lock_guard<std::mutex> l(g_lock);
    if (g_tried) return g_ok && rate == g_rate && channels == g_ch;
    g_tried = true;
    if (getenv("XRT_NO_AUDIO")) { LOG(1, "[audio] host output disabled (XRT_NO_AUDIO)"); return false; }
    g_rate = rate; g_ch = channels;
    // ring: half a second (only the target latency is ever used; the rest absorbs scheduling jitter)
    if (ma_pcm_rb_init(ma_format_s16, (ma_uint32)channels, (ma_uint32)(rate / 2), nullptr, nullptr, &g_rb) != MA_SUCCESS) {
        LOG(0, "[audio] ring buffer allocation failed - silent");
        return false;
    }
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_s16;
    cfg.playback.channels = (ma_uint32)channels;
    cfg.sampleRate = (ma_uint32)rate;          // miniaudio resamples / remaps if the device differs
    cfg.dataCallback = callback;
    cfg.periodSizeInMilliseconds = 10;
    cfg.performanceProfile = ma_performance_profile_low_latency;
    if (ma_device_init(nullptr, &cfg, &g_dev) != MA_SUCCESS) {
        LOG(0, "[audio] no playback device could be opened - silent");
        ma_pcm_rb_uninit(&g_rb);
        return false;
    }
    if (ma_device_start(&g_dev) != MA_SUCCESS) {
        LOG(0, "[audio] playback device failed to start - silent");
        ma_device_uninit(&g_dev); ma_pcm_rb_uninit(&g_rb);
        return false;
    }
    g_ok = true;
    LOG(1, "[audio] output: %s, %d Hz %d ch (device %u Hz %u ch, %s)", g_dev.playback.name, rate, channels,
        g_dev.playback.internalSampleRate, g_dev.playback.internalChannels, ma_get_backend_name(g_dev.pContext->backend));
    return true;
}

size_t audio_host_queued_frames() { return g_ok ? (size_t)ma_pcm_rb_available_read(&g_rb) : 0; }

size_t audio_host_target_frames() {
    static size_t t = 0;
    if (!t) {
        const char *e = getenv("XRT_AUDIO_LATENCY_MS");
        int ms = e ? atoi(e) : 60;
        if (ms < 10) ms = 10;
        if (ms > 400) ms = 400;
        t = (size_t)g_rate * (size_t)ms / 1000;
    }
    return t;
}

void audio_host_write(const int16_t *pcm, size_t frames) {
    if (!g_ok) return;
    g_running.store(true, std::memory_order_relaxed);
    while (frames) {
        ma_uint32 n = (ma_uint32)frames;
        void *dst = nullptr;
        if (ma_pcm_rb_acquire_write(&g_rb, &n, &dst) != MA_SUCCESS || n == 0) break;   // full: drop the rest
        std::memcpy(dst, pcm, (size_t)n * g_ch * sizeof(int16_t));
        ma_pcm_rb_commit_write(&g_rb, n);
        pcm += (size_t)n * g_ch; frames -= n;
    }
}

void audio_host_stats() {
    if (!g_ok) return;
    LOG(1, "[audio] %llu frames played, %llu underruns", (unsigned long long)g_played.load(), (unsigned long long)g_underruns.load());
}
