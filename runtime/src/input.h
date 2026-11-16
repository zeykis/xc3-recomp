#pragma once
#include <cstdint>
uint64_t input_buttons();            // NpadButton bits (A=0, B=1, X=2, Y=3, ... Down=15)
void     input_sticks(int32_t out[4]); // lx, ly, rx, ry
void     input_key(int vk, bool down); // Windows virtual-key code
void     input_release_all();         // window lost focus
