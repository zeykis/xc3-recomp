# XC3 Recomp — Architecture

Goal: a native PC executable of Xenoblade Chronicles 3, produced by **static recompilation** of
the Switch executable (AArch64 → C), plus a **high-level reimplementation** of the Switch APIs the
game calls (OS, filesystem, input, audio, and NVN graphics → Vulkan).

The game files (executable, assets) are never redistributed: the user supplies their own dump, and
the generated code is created locally from it.

## Pipeline

```
XCI/NSP ──triage.py/hactool──► exefs/main (NSO) ──► game/main.elf
                                                          │
                                       recomp/recompile.py│  (Python + capstone)
                                                          ▼
                                   gen/main_000..046.c  (70,666 C functions, ~430 MB)
                                   gen/functable.c      (guest address → C function)
                                                          │
              runtime/ (loader, dispatch, HLE) ───────────┤  CMake + clang
                                                          ▼
                                                        xc3(.exe)  ── loads game/main.elf + romfs at runtime
```

## Key decisions

**1. Recompile only `main`; reimplement everything else.**
`main` (16 MB of code) contains the whole game + engine. It reaches the outside world only through
410 dynamic imports (nn::os/fs/hid/audio/…, libc) and ~520 NVN GPU entry points obtained via
`nvnBootstrapLoader`. No syscalls (`svc`) and no system-register access exist in `main`. So the
SDK modules (`sdk`, `subsdk0`, `rtld`) are *not* recompiled — their APIs are implemented natively.
This is the same "HLE boundary" approach N64Recomp/XenonRecomp-based ports use, and it means no
Switch kernel, IPC, or GPU command-buffer emulation is needed.

**2. Guest addresses are host addresses.**
The image is loaded into host memory and relocated to wherever it lands (`G_BASE`). Pointers inside
the game are therefore real host pointers: loads/stores compile to plain memory accesses, host
allocators can hand memory directly to the game, and HLE functions read guest structs in place.
Both AArch64 and x86-64 are 64-bit little-endian, so layouts match.

**3. One C function per guest function, exact boundaries from `.eh_frame`.**
The NSO ships an `.eh_frame_hdr` table with 70,295 function entries; together with BL targets and
code pointers in relocations this gives 70,666 functions. Inside a function, branches become
`goto`; calls become direct C calls; jump tables (886 resolved) become `switch`; everything else
indirect (`blr`, vtable calls, tail calls through registers) goes through `xrt_dispatch`, a
direct-mapped table lookup.

**4. CPU state lives in a `Ctx` struct** (x0–x30, SP, v0–v31 as 128-bit unions, NZCV, exclusive
monitor). Correctness first; a later optimisation pass can cache registers in locals.

**5. Every instruction form is verified against a reference emulator.**
`tests/difftest.py` takes every distinct instruction form used by the game (1,059 forms), runs each
on random register/flag/memory states through both the generated C and Unicorn, and compares all
architectural state. Current status: 0 mismatches. Intentional deviations: `frecpe/frsqrte` return
exact values instead of 8-bit estimates; exclusives (`ldxr/stxr`) are implemented as CAS.

**6. HLE layer is incremental and self-reporting.**
Imports resolve to `HLE_BASE + index`. Unimplemented ones log once and return 0 (or abort with
`XRT_STRICT=1`); `hle_print_stats()` lists what the game actually needed. Guest SDK objects
(mutexes, events, threads) are keyed by their guest address and backed by host primitives. NVN
functions are registered dynamically by name (`nvn:<function>`).

## Repository layout

| Path | Purpose |
|---|---|
| `recomp/a64c.py` | Instruction translator (AArch64 → C statements) |
| `recomp/recompile.py` | Driver: functions, labels, jump tables, file splitting, coverage report |
| `runtime/include/xrt.h` | Ctx + helpers used by generated code |
| `runtime/src/loader.c` | Image loading, relocations, import binding, dispatch, host→guest calls |
| `runtime/src/hle.c` | Import registry, libc functions, guest `printf` |
| `runtime/src/hle_nn.cpp` | nn::os / fs / oe / vi / mem / NVN bootstrap |
| `runtime/src/platform.cpp` | Memory, time, big-stack threads (Windows + Linux) |
| `tests/difftest.py` | Instruction-level differential testing vs Unicorn |
| `tools/scan.py` | Scope analysis of the executable |
| `analysis/` | Generated reports (coverage, scan, difftest, imports) |
| `game/` | **Your** game files (gitignored): main.elf, sdk.elf, romfs/ |
| `gen/` | Generated C (gitignored, rebuild with `recompile.py`) |
