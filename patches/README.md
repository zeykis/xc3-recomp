# Code patches (XC3 2.2.1)

IPSwitch `.pchtxt` patches for the game's executable, version 2.2.1 (`@nsobid-82D187FE…`). `recomp/recompile.py`
applies them before translating the code (a game folder's own `game/patches/` replaces this folder when it exists).

| File | What | From |
|---|---|---|
| `60fps_2.2.0.pchtxt` | 60 fps in the field (dynamic), 30 fps cutscenes | community emulator mod |
| `hfr_2.2.0.pchtxt` | High frame rate on top of the 60 fps patch: the frame cap is the runtime's (TAB menu) | this project |
| `res_2.2.0.pchtxt` | Render resolution base (experimental) | this project |
| `mods/*.pchtxt` | Graphics mods, built in both ways and switched on / off in game (TAB menu) | community emulator mods |

The community patches are kept exactly as their authors published them (credit to them); they contain small
instruction patches only, no game code or data.
