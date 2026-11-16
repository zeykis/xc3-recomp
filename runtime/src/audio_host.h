// Host audio output (audio_host.cpp). 16-bit interleaved PCM.
#pragma once
#include <cstddef>
#include <cstdint>

bool   audio_host_open(int rate, int channels);   // once; false = no device (stay on the wall-clock model)
size_t audio_host_queued_frames();                // frames waiting in the ring
size_t audio_host_target_frames();                // how many to keep queued (latency)
void   audio_host_write(const int16_t *pcm, size_t frames);
void   audio_host_stats();
