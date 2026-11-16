# XC3 Recomp — Roadmap

Each milestone ends with something you can run and measure. Status as of 2026-11-09: **M0–M4 done. M5 (PC port: performance, frame rate, options) is next.**

## M0 — Extraction & scoping ✅
- [x] Decrypt XCI with the user's own keys, extract ExeFS without unpacking the 15 GB RomFS
- [x] NSO → ELF conversion (`triage.py`), dynamic symbol/import analysis
- [x] Scope: 4.1M instructions, 70,666 functions, 199 mnemonics, 0 syscalls, 410 imports + ~520 NVN entry points

## M1 — Recompiler core ✅
- [x] AArch64 → C translator covering 100% of instructions in `main` (1,059 distinct forms)
- [x] Function boundaries from `.eh_frame`, jump tables → `switch` (all 1,202 resolved)
- [x] Differential testing vs Unicorn: 0 mismatches across all forms
- [x] Exclusive-access (atomics) unit test
- [x] All 48 generated files compile with clang

## M2 — Boot to the game loop ✅
- [x] Loader: relocations, import binding, data imports from sdk.elf, direct-mapped dispatch
- [x] HLE: libc, guest printf/varargs, libc++ (string/sort/shared_ptr/mutex/condvar/chrono), nn::os
      (threads with real ThreadType state, mutexes, condvars, events, timer events, TLS, fibers,
      virtual memory with the Switch's physical budget), nn::fs (RomFS/save mapped to host folders),
      nn::oe, nn::settings (language), nn::mem, nn::vi, nn::hid (virtual Pro Controller), nn::account,
      nn::aoc, nn::audio (silent AudioOut timing model, Opus decoder stub), headless NVN
- [x] `nninitStartup` → all 805 static initializers → `nnMain`
- [x] All 1,202 jump tables recovered; PLT stubs as entry points; runtime feedback loop for extra entries
- [x] Archive (`bf3.arh/.ard`) + zstd decompression verified byte-exact with the game's own code
- [x] Wwise audio engine fully initialised (banks, IO thread, 48 kHz output)
- [x] Fonts, layouts, bdat tables load; title flow reaches PlayReport (title screen logic)
- [x] **Exit criterion met:** main loop runs stably for minutes, ~30 fps paced (`[nvn] N frames presented`)
- Note: the game asks for `menu/font/bf3_<region>.wisty`; the archive has no `_us` variant, so the
  runtime reports **en-GB** as the system language by default (`XRT_LANG=fr|de|it|es|ja|...` to change)

### Debug aids added during M2
- `analysis/trace_funcs.txt` (+ `XRT_TRACE_STR`, `XRT_TRACE_DUMP`, `XRT_TRACE_VEC`) — log entry of chosen functions
- `XRT_WATCH=substr` — report which guest code builds a string (sprintf/strcpy/strcat)
- `XRT_FSLOG=1` — log every file open/read
- Crash handler prints the recompiled function, guest registers and recent indirect calls

## M3 — Graphics: NVN → Vulkan ✅
Vulkan is loaded at runtime (`vulkan-1.dll`), headers are vendored in `third_party/vulkan`: no SDK needed.

- [x] **M3.1 Window + present**: Win32 window, Vulkan device/swapchain (`runtime/src/gpu_vk.cpp`);
      NVN command buffers are recorded per guest command buffer and replayed at `nvnQueueSubmitCommands`
      (including nested `CallCommands`); render-target textures become VkImages; colour/depth clears run
      on the GPU; the presented window texture (1920×1080 RGBA8) is blitted to the swapchain.
      Headless runs can dump frames: `XRT_DUMP_FRAMES=N` → `frames/frame_NNNNN.ppm`
- [x] M3.2 GPU memory: NVN memory pools are imported into Vulkan (`VK_EXT_external_memory_host`), so vertex /
      index / uniform / storage buffers are (pool buffer, offset) with no copies
- [x] M3.3 Textures: format table + Tegra block-linear deswizzle (BC1/4/5/6H/7, RGBA8, R8, ...), VkImages with
      mips / arrays / cubes / 3D, texture and sampler pools, texel buffers, storage images.
      `XRT_DUMP_TEXTURES=N` saves the first N bound textures as `textures/*.dds`
- [x] M3.4 Pipeline state + draws: program, vertex attribs/streams/buffers, uniform buffers, bound textures,
      samplers, blend/colour/channel-mask/depth/polygon state, viewport/scissor are recorded per command
      buffer and replayed into Vulkan; pipeline cache keyed by the full state; dynamic rendering;
      quads expanded to triangles; NVN's lower-left origin emulated with a flipped viewport.
      **First picture: the game's own UI (auto-save notice) renders correctly.**
- [x] M3.5 Shader translator: **Eden's Maxwell → SPIR-V recompiler vendored** (`third_party/shader_recompiler`,
      GPL-3.0) with small shims (`third_party/eden_common`). `XRT_DUMP_SHADERS=1` captures every shader the
      game loads; `xc3-shaderc game/shaders out/` translates them offline.
      **2,576 / 2,578 captured shaders translate, and all output passes `spirv-val`** (Vulkan 1.2 +
      scalar block layout). 2 fail on an unimplemented attribute read.
      Runtime environment answers constant-buffer and texture-type queries; transform feedback and alpha test
      go through Eden's own RuntimeInfo.
- [x] M3.6 Full frames: deferred lighting, post-processing, particles (transform feedback), skinning
      (rasterizer-discard vertex shaders), motion blur / velocity, dynamic resolution, HUD
- [x] **Exit criterion met (2026-11-09):** title screen, opening cutscene, field and battles render; the user
      played through them with no rendering issue reported

### M3 closing work (2026-11)
- Window origin mode 1 = **upper-left**: viewport rect is top-left based (flip `y + h`, negative height),
  scissor and present crop unflipped; `y_negate` false, as in Eden
- NVN BindProgram applies only the stages in its mask (vertex 1, fragment 2, geometry 4, TCS 8, TES 0x10)
- Rasterizer-discard draws (vertex shaders used as compute: skinning) run with `rasterizerDiscardEnable`,
  then end the pass (full barrier)
- Transform feedback: layout from the last geometry stage's control section (`+0xdc` slots, `+0x4dc`
  components, `+0x6dc` stride, `+0x6e8` count); byte counter at control buffer +4; `DrawTransformFeedback`
  = `vkCmdDrawIndirectByteCountEXT`
- NVNvertexAttribState (4 bytes) carries an encoding written by its setters (`0xA5<<24 | stream<<20 |
  format<<12 | offset`): copies made by the game decode correctly (root cause of NaN effects / shards)
- Feedback loops (a draw sampling its own render target) read a copy, as Eden does
- GPU timestamps report a GPU with headroom, so the game's dynamic resolution goes up to 1280×720
- `nvnCommandBufferBarrier` ends the render pass (full barrier); draws whose shaders write storage
  buffers / images do too (`XRT_NO_BARRIER`, `XRT_NO_MEMW_BARRIER`)
- NVN enums corrected from Monado's verified tables (see below):
  - wrap modes: 0 CLAMP, 1 REPEAT, 2 MIRROR_CLAMP, 3 MIRROR_CLAMP_TO_EDGE, 4 MIRROR_CLAMP_TO_BORDER,
    5 CLAMP_TO_BORDER, 6 MIRRORED_REPEAT, 7 CLAMP_TO_EDGE (REPEAT textures were mirrored before);
  - blend factors: dual-source at 0x10–0x13, constant colour at 0x61–0x64.
- New: polygon offset, alpha test, logic op, sampler border colour (`XRT_NO_DEPTH_BIAS`,
  `XRT_NO_ALPHA_TEST`, `XRT_NO_LOGIC_OP`)
- Still open (minor): MSAA, line width, vertex divisors > 1, custom border colours (approximated)

### Test tooling
- `runner.bat`: leave it open; it runs `runner\job.txt` (BUILD, TIMEOUT, CLEAN, `XRT_*` only) and writes
  `runner\status.txt`, `run-log.txt`, `build-log.txt`. It refuses to start while Eden or the game runs.
- `XRT_CAPTURE_AT=s1,s2` (F12 captures at those seconds), `XRT_EXIT_AT=s`, `XRT_AUTOPRESS=BUTTON@s[:hold]`.
  Into gameplay from a save: `A@60,DOWN@95,A@98,A@103,...` (every 5 s to ~133)
- F12 capture: `frames/draw_list_<frame>.txt`, per-draw diffs, render targets, `debugdraw/` for effect draws
- Reference: RenderDoc captures of Eden 1.0.0 in `reference/` (never committed)

### Reference: Monado (XCDE recomp)
[Monado](https://github.com/TheMonadoGuy/Monado) (MIT), a static recompilation of *Xenoblade Chronicles:
Definitive Edition*, same engine family and platform. Used as a reference for NVN semantics and its list of
graphics bug classes (`docs/LESSONS_FOR_XC2.md` there). Candidates for later milestones: its FFmpeg `movie::`
implementation and audio renderer (M4), frame limiter / frame pacing, pipeline precompilation, FSR / DLSS (M5).
Any code taken from it keeps its MIT notice.

### Game progress during M3 (CPU side)
- Keyboard input in the window + `XRT_AUTOPRESS="A@50,A@55"` scripted presses (`runtime/src/input.cpp`)
- With input the game goes title screen → new game → opening movie → **3D gameplay rendering
  (~1,500 draws/frame, ~20–27 fps on a software Vulkan driver)**
- Opening movie: `movie::Extractor/Decoder` are given virtual tables that report failure, so the game
  skips the video (real playback is M4). nn::os message queues implemented.
- Recompiler: jump tables whose base register is restored in an epilogue on another path are now resolved
  (found via a runtime "not a function start" stop)
- `XRT_WATCHDOG=N`: every N seconds, print what every guest thread is doing (finds hangs)

### Shaders
- 3,847 shaders seen at boot (1,895 vertex, 1,855 pixel, 83 geometry), median 80 instructions
- Captured blob = 0x30-byte NVN header (magic `0x12345678`) + 0x50-byte SPH + Maxwell code; the
  translator gets blob offsets, so code starts at 0x80 (scheduling words stay 32-byte aligned)
- `tools/shader_scan.py game/shaders` prints stage counts / sizes / opcode histogram

### Findings while drawing the first frame
- NVN uniform buffer index *i* = hardware constant buffer bank *i*+3 (banks 0-2 belong to the driver)
- Bank 0 (driver): storage buffer table at `0x110 + 0x400*NVNstage + 16*slot` = (u64 address, u32 size)
- Bank 1: the shader's literal constants, stored in the code memory at control-section `[0x700]..[0x704]`
- Title screen, menus and loading screen render correctly on an RTX 3080 Ti at ~26 fps
- Bound textures = 64-bit handles in bank 2 at `0x20 + 8*slot`; handle = `sampler << 20 | texture`
- NVNvertexAttribState is 4 bytes; NVN enums mostly follow OpenGL order (blend func 5/6 = src alpha / 1-src alpha,
  depth func 1..8 = never..always, primitive 4 = triangles, 7 = quads, index type 1 = u16)
- Depth mode 0 = OpenGL clip space (-1..1, converted in the shader); window origin mode 1 = upper-left (see M3 closing work)
- Eden only declares a vertex input when the "previous stage" stores it: the host must mark all of them
- Debug: `XRT_DEBUG_DRAW=frame,n` dumps one draw (state, buffers, SPIR-V); `XRT_DEBUG_NOBLEND`, `XRT_DEBUG_NOCULL`,
  `XRT_NO_DRAW`, `XRT_FLIP_Y=0/1`

### Findings so far
- The game records its frame into secondary command buffers and calls them from a primary one
  (`nvnCommandBufferCallCommands`), then submits the primary. It only submits a command buffer whose
  `GetCommandMemoryUsed()` is non-zero.
- `NVNtextureView` objects are embedded in the game's own structs with other fields right after them:
  the runtime must never write into guest NVN structs (side tables keyed by address instead).
- Window textures: 2 × RGBA8 (format 37) 1920×1080.

### Debug switches
`XRT_NVNLOG=N` or `FROM,N` (log every recorded op of those frames), `XRT_NVNTRACE=N` (first N NVN calls with
arguments), `XRT_HLEFIRST=1` (first call of every SDK function), `XRT_DUMP_FRAMES=N`, `XRT_HEADLESS=1`
(no window), `XRT_NO_GPU=1` (old headless mode), `XRT_VK_VALIDATION=1` (Vulkan validation layer)

## M4 — Input, audio, video ✅
- [x] Input: keyboard in the window + XInput gamepad → virtual Pro Controller (`runtime/src/input.cpp`)
- [x] Audio (2026-11-09): the game (Wwise) mixes itself and outputs 16-bit PCM through `nn::audio::AudioOut`;
      `runtime/src/audio_host.cpp` plays it with miniaudio (WASAPI / PulseAudio / ALSA), the sound card's clock
      pacing buffer release (60 ms target, `XRT_AUDIO_LATENCY_MS`). `nn::codec::HardwareOpusDecoder` decodes
      with libopus 1.5.2 (Switch framing: 8-byte header, big-endian size). Verified: 48 kHz, 0 underruns,
      0 decode errors over a 4-minute run. `XRT_NO_AUDIO`, `XRT_NO_OPUS` for A/B
- [x] Video (2026-11-09): `movie::Extractor` / `Decoder` / `MediaData` / `Buffer` (`runtime/src/hle_movie.cpp`) on
      FFmpeg 8.1 (libavformat demux, native VP9 decoder). 238 WebM / VP9 cutscenes, 1280×720 30 fps, no audio
      track (their sound is Wwise's). Frames go to the game as NV12 (stride = width, UV after the Y plane: the game
      passes a 1,382,400-byte buffer of its own and reads it). Design follows Monado's movie HLE; vtable slots are
      XC3's (subsdk0 `_ZTVN5movie7DecoderE`). `XRT_NO_MOVIE=1` skips videos, `XRT_LOG_MOVIE=1` traces every call.
- [x] `nn::os` multi-wait (`WaitAny` over events): the movie player's decoder thread waits with it; it was
      unimplemented (returned immediately), so no frame was ever requested
- [x] **Exit criterion met (2026-11-09):** playable with keyboard / controller, with sound, music, voices and
      cutscene videos (the opening movie plays and skips; verified by the user)

## M5 — PC port polish
- [ ] Performance: register caching in locals, -O2 on hot functions, profile-guided
- [ ] Resolution / FPS unlocks (the game's frame limiter, frame-count thresholds), widescreen UI, mouse/keyboard
- [ ] Shader / pipeline precompilation (no first-sight stutter), upscalers
- [ ] Save data location, settings UI, Windows packaging
- [ ] Support the latest game update (2.x) — rerun the pipeline on the updated `main`

## Working loop (how each session moves forward)
1. `./build.sh && cd game && ../build/xc3 --game .`
2. Read the first FATAL / UNIMPLEMENTED line
3. Implement that instruction / HLE function (with a test when it's CPU semantics)
4. Repeat. `hle_print_stats()` at exit lists every import the game needed.
