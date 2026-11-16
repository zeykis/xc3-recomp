// In-game settings overlay (TAB), Dear ImGui. The window thread feeds it input (ui_on_msg); the presenter builds a
// frame (ui_frame) and draws it on top of the game image (gpu_vk.cpp).
#pragma once
#include <cstdint>
struct ImDrawData;

bool ui_on_msg(void *hwnd, unsigned msg, uintptr_t wparam, intptr_t lparam);   // window thread: true = consumed
bool ui_visible();                                  // overlay open (the game gets no keyboard)
void ui_init(int win_h);                            // presenter thread: ImGui context + fonts (before the backend)
ImDrawData *ui_frame(int w, int h, double fps, int src_w, int src_h);   // nullptr = nothing to draw this frame
// gpu_vk.cpp: apply window mode / size from settings() (posted to the window thread)
void gpu_window_apply();
bool gpu_fg_active();
int gpu_display_hz();
