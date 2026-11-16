# XC3 Recomp

Static recompilation of *Xenoblade Chronicles 3* (Nintendo Switch) into a native PC executable.

> **Status: work in progress.** The recompiled game runs and renders through Vulkan: title screen,
> cutscenes, the field and battles, with keyboard or gamepad. There is sound and cutscene video. Performance
> is not optimised yet (milestone M5). See [Status](#status) and
> [docs/ROADMAP.md](docs/ROADMAP.md).

How it works: every function of the game's Switch executable (ARM64) is translated into C, which
compiles into a normal PC program. Everything the game asks of the Switch itself (memory, threads,
files, input, the NVN graphics API) is reimplemented natively. Details in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

**No game code or assets are included.** You need your own dump of the game and the keys from your
own console. The `game/` and `gen/` folders contain material derived from Nintendo's code: never
publish them (they are in `.gitignore`).

---

## Quick start (Windows)

### 1. Install the tools (one time)

| Tool | Install |
|---|---|
| Python 3.10+ | `winget install Python.Python.3.12` |
| LLVM (clang) | `winget install LLVM.LLVM` |
| CMake | `winget install Kitware.CMake` |
| Visual Studio Build Tools (Microsoft linker + Windows SDK) | `winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"` |

Ninja and the Python packages are installed automatically by the build script. Open a **new**
terminal after installing anything, so the PATH is updated.

### 2. Install your game files

You need your own copy of the game: the base game (`.xci` / `.nsp`), **update 2.2.0** (`.nsp` / `.nca`; the code is
recompiled for that exact version) and, if you have it, the DLC — or simply a copy installed in Eden / a yuzu-based
emulator, which is found automatically (with its saves) — plus `prod.keys` dumped from your own console.

```
build-windows.bat tools
build-cmake\xc3-setup.exe
```

The first command builds the setup and the launcher (no game files needed yet) and downloads
[hactool](https://github.com/SciresM/hactool) (pinned release, checksum verified). In the setup, add your files, pick
`prod.keys`, keep **this folder** as the install folder (it is filled in when the setup runs from `build-cmake\`) and
press *Install*: the game files go to `game\` (about 20 GB; `main.elf`, `sdk.elf`, `romfs\`, `aoc\`).

### 3. Build

```
build-windows.bat
```

It runs five steps and stops with the exact fix if something is missing:

1. **Python**: installs `capstone` (instruction decoder) and `ninja` (build tool) with pip.
2. **clang**: finds LLVM, including `C:\Program Files\LLVM\bin` if it isn't on PATH.
3. **Visual Studio**: loads the C++ build environment (linker + Windows SDK).
4. **Generated code**: runs `recomp\recompile.py`, which turns `game\main.elf` (with the patches in `patches\`) into
   ~430 MB of C in `gen\` (~1 min).
5. **Build**: compiles everything into `build-cmake\xc3.exe` (FFmpeg for the cutscenes is downloaded once, pinned and
   checksum verified). The first build takes 10–30 min depending on your CPU; later builds only recompile what changed.

### 4. Play

```
make_playable.bat
```

copies the build to `..\XC3-Playable\`; start **`XC3 Launcher.exe`** there: *Play*, or *Play Randomized* (below).

### Running it directly

```
cd game
..\build-cmake\xc3.exe --game . --log 1
```

`--log 2` or `--log 3` prints more detail (every SDK call at level 3). What you should see today:

```
[boot] nninitStartup ...
[boot] running 805 static initializers
[boot] nnMain ...
[fs] mount rom: -> ./romfs
[os] thread 'AK::...' started ...     (Wwise audio + the game's job threads)
[nvn] 300 frames presented (29.x fps avg)
[nvn] 600 frames presented ...
```

The game opens a window (Vulkan) and renders: title screen, cutscenes, field and battles, at roughly
18–30 fps for now (performance is milestone M5). Sound plays on the default output device; cutscene videos play through FFmpeg. Close the
window to quit.

Controls (click the game window first):

| Switch | Keyboard | Xbox-style gamepad (XInput) |
|---|---|---|
| A / B / X / Y | Enter or Z / Backspace or X / C / V | B / A / Y / X (by position, like a Pro Controller) |
| L / R | Q / E | LB / RB |
| ZL / ZR | 1 / 3 | LT / RT |
| + / − | Esc or P / Shift | Start / Back |
| Left / right stick | WASD / IJKL | sticks |
| Stick clicks | F / H | stick clicks |
| D-pad | arrow keys | D-pad |

Keyboard and gamepad work together; held keys are released when the window loses focus.

**Settings (TAB):** TAB opens an overlay (mouse or keyboard; TAB / Esc closes it) with the window size and borderless
fullscreen, the internal 3D resolution (×960×544; applies after a restart), the frame cap and the menu frame cap,
VSync mode, the upscaler from the game image to the window (bilinear or AMD FSR 1 with RCAS sharpness), volume, and a
frame-rate counter. **Save** writes them to `xc3.ini` in the game folder (plain `key = value`, editable by hand;
`XRT_FPS_CAP`, `XRT_MENU_FPS`, `XRT_RES_SCALE`, `XRT_PRESENT_MODE`, `XRT_UPSCALER`, `XRT_VOLUME`, `XRT_WINDOW=WxH|borderless`
override it). Alt+Enter toggles borderless fullscreen.

**Graphics mods (TAB):** IPSwitch patches in `patches\mods\` (one `.pchtxt` per mod, e.g. the emulator mods
Remove Motion Blur, Remove Outlines, Improve Shadows, Improve Level of Detail, Improved DOF, Emu Scaler Enabler, ...)
are built into the exe both ways at the next build and switched on or off at run time in the TAB menu (saved as
`mod.<name> = 1` in `xc3.ini`; `XRT_MODS=all|none|name,name` overrides). A patched `bl` into a code cave of the mod runs
the cave in place, so mods whose caves collide with the 60 fps patch (Improve Shadows) still work. Mods already part
of the permanent patches (`patches\*.pchtxt`: 60 fps, high frame rate, resolution) are skipped. A game folder's own
`game\patches\` replaces `patches\` when it exists (local experiments).

**Installer:** `make_installer.bat [nobuild] [zip]` packages `dist\XC3-Recomp-Setup\` (`XC3 Setup.exe` + `files\`: xc3.exe,
DLLs, hactool, patches and mods, shader cache). The setup takes the player's own files - base game `.xci`/`.nsp`,
update 2.2.0 and DLC `.nsp`/`.nca`, or what Eden / yuzu-based emulators have installed (found automatically, with
their saves) - and `prod.keys` (title keys come from the `.nsp` tickets or `title.keys`). It identifies each file by
its NCA header, checks that the update's code is the version the exe was recompiled for (`gen/build_info.h`),
extracts the ExeFS / RomFS (update applied) and DLC with hactool, converts the executables to ELF, copies the program
and makes desktop / Start menu shortcuts. An installed `xc3.exe` started without `--game` runs from the `game\`
folder next to it and writes its log to `game\play-log.txt`.

**Mods:** mods made for emulators go in the game folder's `mods\` folder, one folder per mod, as Eden / yuzu want
them in their `load\010074F013262000\` folder: `mods\<name>\romfs\...` (or the files directly in `mods\<name>\`).
Their files replace the game's: a file a mod has is hidden from the game's archive (`bf3.arh` / `bf3.ard`), so the game
loads it loose (the method of xc3-file-loader), for the base game and the DLC packs alike. Mods are turned on and off in
the TAB menu (`romfs_mod.<name>` in `xc3.ini`); files the game already loaded change at the next area load or restart.
A mod's code patches (`mods\<name>\exefs\*.pchtxt`) become switchable at the next build, like `patches\mods`.

**Launcher (normal / randomized):** `XC3 Launcher.exe` (next to `xc3.exe`; the installer's shortcuts open it) has
*Play* and *Play Randomized*. Each has its own saves (`game\save`, `game\save_randomized`). *Play Randomized* turns
on the files of SonahSofern's [Xenoblade-Series-Randomizer](https://github.com/SonahSofern/Xenoblade-Series-Randomizer):
*Open Randomizer...* downloads its release the first time (1.4.2, 108 MB, checksum verified, into `Randomizer\` next to
the launcher; or locate a copy you have), points its output folder at `game\mods\Randomizer` and starts it — choose
Xenoblade Chronicles 3, make a seed, then *Play Randomized*. A new seed is the randomizer run again. Underneath,
`xc3.exe --profile <name>` reads `game\profiles\<name>.ini` after `xc3.ini` (`save_dir`, `romfs_mod.<mod> = 1` for the
mods only that profile uses, any other setting).

**Playable copy:** `make_playable.bat` copies the current build to `..\XC3-Playable` (or `make_playable.bat <folder>`) with a
`Play XC3.bat` that starts it on this repo's `game\` folder; `make_playable.bat <folder> full` copies `game\` too
(about 20 GB) for a copy that doesn't depend on the repo.
`run.bat` starts the game in its window with no time limit (log in `game\run-log.txt`).
Pressing Enter a few times goes title screen → New Game → opening movie (+ skips it) → gameplay. F12 saves a frame
capture (render targets + draw list) to `game\frames\`.

Options (environment variables, e.g. `set XRT_LANG=fr` before running):

| Variable | Effect |
|---|---|
| `XRT_LANG=fr` | System language (`ja`, `en-GB` default, `fr`, `de`, `it`, `es`, `ko`, `zh-CN`, `zh-TW`). `en-US` doesn't work: the game has no US font file. |
| `XRT_FSLOG=1` | Log every file the game opens/reads |
| `XRT_WATCH=text` | Report which game code builds a string containing `text` |
| `XRT_DUMP_FRAMES=300` | Every 300th frame, save the game's output image (`frames\frame_NNNNN.ppm`) and exactly what the window shows (`frames\window_NNNNN.ppm`) |
| `XRT_HEADLESS=1` | Don't open a window |
| `XRT_NVNLOG=2000,1` | Log every graphics command of frame 2000 |
| `XRT_DUMP_TEXTURES=500` | Save the first 500 textures the game uses to `textures\*.dds` |
| `XRT_AUTOPRESS=A@50,A@55` | Press A at 50 s and 55 s (scripted input) |
| `XRT_NO_MOVIE=1` | Skip cutscene videos (`XRT_LOG_MOVIE=1` traces the video calls) |
| `XRT_NO_AUDIO=1` | No sound output (also `XRT_AUDIO_LATENCY_MS=60` to change the output latency) |
| `XRT_WATCHDOG=30` | Every 30 s, print what each game thread is doing |
| `XRT_DUMP_SHADERS=1` | Save every GPU shader the game loads to `shaders\` (then: `..\build-cmake\xc3-shaderc.exe shaders spv`) |

Needs a GPU driver with Vulkan 1.2 (any NVIDIA/AMD/Intel driver from the last few years).

---

## Troubleshooting (Windows)

| Message | Fix |
|---|---|
| `CMake was unable to find a build program corresponding to "Ninja"` | Use `build-windows.bat` instead of calling cmake directly; it installs Ninja and passes its path to CMake. |
| `\Microsoft was unexpected at this time.` | You have an old copy of `build-windows.bat`. Get the current one (it no longer uses parenthesized blocks around the `Program Files (x86)` path). |
| `Visual Studio Build Tools not found` / `C++ build tools are missing` | Install the Build Tools command above, or in *Visual Studio Installer* → *Modify* → tick **Desktop development with C++**. |
| `clang not found` | `winget install LLVM.LLVM`, then open a new terminal. |
| `game\main.elf missing` | Install the game files (step 2): `build-windows.bat tools`, then `build-cmake\xc3-setup.exe`. |
| `FATAL: indirect jump to main+0x... which is not a function start` | The game reached code the recompiler didn't know was a function. The address was appended to `game\extra_entries.txt` (read by the next build): run `build-windows.bat` again. Please report it (or add it to `recomp\entries\82D187FE9EF9BE92.txt` in a pull request). |
| `[fs] missing file rom:/bf3.arh` | The game files aren't installed: run the setup (step 2). |

---

## Linux / WSL

```sh
pip install capstone
python3 recomp/recompile.py
cmake -B build-cmake -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build-cmake
cd game && ../build-cmake/xc3 --game . --log 1
```

For quick iteration there is also `./build.sh`, which recompiles only the changed generated files
and links `build/xc3`.

---

## Development loop

1. Run the game and read the first `FATAL` or `UNIMPLEMENTED` line.
2. Implement it:
   - a missing Switch/SDK function → `runtime/src/hle_*.cpp` (registered by its mangled name);
   - a CPU instruction issue → `recomp/a64c.py`, then re-run `recompile.py` and the tests;
   - an unknown function entry → `recomp/entries/<build id>.txt` (or `game/extra_entries.txt`), then re-run `recompile.py`.
3. Rebuild and repeat. At exit, the runtime prints every SDK import the game called but that isn't
   implemented yet.

### Tests

```sh
python3 tests/difftest.py      # every instruction form the game uses vs the Unicorn reference emulator (needs: pip install unicorn)
clang -Iruntime/include tests/test_exclusive.c -o /tmp/t && /tmp/t   # atomic load/store pairs
```

---

## Project layout

| Path | Purpose |
|---|---|
| `build-windows.bat` | Windows build: prerequisites, code generation, CMake build |
| `CMakeLists.txt` | Build definition (`xc3gen` = generated code library, `xc3` = executable) |
| `recomp/a64c.py` | ARM64 → C instruction translator |
| `recomp/recompile.py` | Recompiler driver: functions, jump tables, output files, coverage report |
| `recomp/hooks.txt`, `recomp/entries/` | Runtime hooks and extra function entries, per game build id |
| `patches/` | Code patches built in: 60 fps (community), high frame rate and resolution (this project); `patches/mods/` = switchable graphics mods |
| `tools/setup/` | `xc3-setup`: installs the game files from your own dump / emulator (and packages a player install, `make_installer.bat`) |
| `tools/launcher/` | `XC3 Launcher`: Play / Play Randomized, downloads the randomizer |
| `runtime/include/xrt.h` | CPU context and helpers used by the generated code |
| `runtime/src/loader.c` | Loads `main.elf`, applies relocations, binds imports, dispatches indirect calls |
| `runtime/src/hle.c` | Import registry, C library functions, guest `printf` |
| `runtime/src/hle_nn.cpp` | Nintendo SDK: threads, sync, memory, files, applet, display, NVN entry |
| `runtime/src/hle_cxx.cpp` | C++ standard library functions the game imports |
| `runtime/src/hle_fiber.cpp` | `nn::os` fibers (Win32 Fibers / ucontext) |
| `runtime/src/platform.cpp` | Windows/Linux memory, time and thread primitives |
| `tools/extract_game.py` | (older path) gets `main.elf`, `sdk.elf` and optionally `romfs/` from a base-game dump |
| `tools/scan.py` | Size/scope analysis of the executable |
| `tests/` | Differential instruction tests and atomics test |
| `docs/` | Architecture and roadmap |
| `game/` | Your game files (not committed) |
| `gen/` | Generated C (not committed; recreated by `recompile.py`) |

---

## Status

As of 2026-11-09.

| | |
|---|---|
| Instructions translated | 4,105,208 / 4,105,208 (100%) |
| Functions | 70,692 |
| Instruction forms verified against a reference emulator | 1,059 / 1,059, 0 mismatches |
| Jump tables recovered | 1,202 / 1,202 |
| Generated C | 48 files, ~430 MB, compiles cleanly on Linux (clang) and Windows (clang + lld-link) |
| Boot | Complete: 805 static initializers → `nnMain` → ~30 job threads, Wwise audio, archive + zstd, fonts/layouts/tables |
| Audio, video (M4) | Done: game audio through WASAPI, Opus decoded with libopus; cutscenes decoded with FFmpeg |
| Graphics (M3) | Done: NVN → Vulkan with Eden's shader recompiler; title, cutscenes, field and battles render |

Next steps (full list in [docs/ROADMAP.md](docs/ROADMAP.md)):

1. **M5 – PC port:** performance, unlocked frame rate, resolution options, shader precompilation.

---

## Licensing

The shader translator in `third_party/shader_recompiler` comes from the [Eden](https://eden-emu.dev)
emulator (a yuzu fork) and is **GPL-3.0-or-later**, as is its SPIR-V emitter `third_party/sirit`.
Because `xc3` links it, the project as a whole is distributed under GPL-3.0-or-later.
Other vendored code: Vulkan headers (Apache-2.0/MIT), SPIR-V headers (MIT), a subset of Boost 1.83
headers (Boost Software License 1.0), fmt 9.1 headers (MIT), libopus 1.5.2 (BSD-3-Clause),
miniaudio 0.11.21 (public domain / MIT-0). FFmpeg 8.1 (LGPL-2.1, shared DLLs, downloaded by
`build-windows.bat`, not committed). Each folder keeps its licence file.
[Monado](https://github.com/TheMonadoGuy/Monado) (MIT) is used as a reference for NVN behaviour; any
code taken from it keeps its MIT notice.
None of this includes anything from the game: `game/` (your dump) and `gen/` (code generated from it)
are never committed.

The code patches in `patches/` that don't say "xc3-recomp" in their header (60 fps, and the graphics mods in
`patches/mods/`) are community emulator mods for XC3 2.2.0, kept as their authors published them; credit to their
authors. hactool (ISC), FFmpeg and Xenoblade-Series-Randomizer are downloaded from their own releases, not included.
