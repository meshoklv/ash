<p align="center">
  <img src="docs/cover.png" alt="ash - Dark Souls 1 engine for the PSP" width="100%">
</p>

<h1 align="center">ash</h1>

<p align="center"><b>A from-scratch game engine in C that runs a cut-down Dark Souls 1 on a real Sony PSP.</b><br>
Your own legally owned game files in, a Memory Stick folder out. 64 MB of RAM, 333 MHz, no Unity, no port - just <code>sceGu</code>.</p>

<p align="center">
  <img src="https://img.shields.io/badge/license-MIT-blue?style=flat-square" alt="MIT license">
  <img src="https://img.shields.io/badge/platform-PSP%20(CFW)-black?style=flat-square" alt="PSP">
  <img src="https://img.shields.io/badge/language-C99-informational?style=flat-square" alt="C99">
  <img src="https://img.shields.io/badge/toolchain-pspdev%20v20261001-orange?style=flat-square" alt="pspdev">
  <img src="https://img.shields.io/badge/runs%20on%20hardware-yes-brightgreen?style=flat-square" alt="runs on hardware">
  <img src="https://img.shields.io/badge/PRs-welcome-brightgreen?style=flat-square" alt="PRs welcome">
</p>

<p align="center">
  <a href="#quick-start">Quick start</a> ·
  <a href="#features">Features</a> ·
  <a href="#how-it-works">How it works</a> ·
  <a href="#status">Status</a> ·
  <a href="README.ru.md">Русский</a>
</p>

---

> [!IMPORTANT]
> **Bring your own legally owned copy of Dark Souls: Remastered.** This repository contains **only original code and tools**: no FromSoftware assets, no extracted data, no ROMs, no copy-protection bypass, no save editors. Every model, texture, animation and parameter the game uses is converted *on your machine* from *your* copy and must never be redistributed. *Ash* is an unofficial fan project, not affiliated with or endorsed by FromSoftware or Bandai Namco; "Dark Souls" is their trademark.

## Real PSP, real numbers

The engine has been run on actual hardware. Measured on a **PSP Street E1000** (CFW 6.60 PRO-C), one uninterrupted session of **26 minutes, no crash** - title screen, character creation, the whole Northern Undead Asylum, the Asylum Demon fight (won), the crow ride, and Firelink Shrine (the full log is in [`docs/hw/`](docs/hw/2026-10-15_E1000_game_8137718_log.txt)):

| | Undead Asylum | Firelink Shrine |
|---|---|---|
| Frame rate, median | **23.8 FPS** (10th percentile 15) | **30 FPS** (10th percentile 30) |
| Heap in use | 11.7 MB (peak 12.3 MB) | 6.1 MB |
| Autosaves through `sceUtilitySavedata` | 22 slot writes (A/B), all OK | |

| Hardware micro-benchmark (`ash_bench`, E1000) | |
|---|---|
| CPU 333 MHz vs 222 MHz | 1.50x |
| 4x4 matrix multiply, VFPU vs FPU | 0.66 us vs 2.08 us |
| Memory Stick read / write | 15.5 MB/s / 4.5 MB/s |
| Free RAM in 64 MB mode / 32 MB mode | 51.9 MB / 23.8 MB |

> [!NOTE]
> No screenshots or photos are shipped: every frame the game draws is built from the owner's game data, and this repo does not redistribute any of it. Build it, cook your own copy, and see it on your own PSP.

## Why

Dark Souls is a 2011 PC/console game with multi-gigabyte assets and a skeleton-heavy renderer. The PSP is a 2004 handheld with 64 MB of RAM, a 333 MHz MIPS CPU and 2 MB of VRAM, and the original has no source code to port. *Ash* asks a different question: **what is the smallest honest engine that keeps the feel** - stamina, weight of attacks, roll i-frames, lock-on, bonfires, bloodstain, boss behind a fog gate - and fits that machine? The gameplay logic is written from scratch; numbers (damage, timings, AI scripts) are read from your own copy at cook time, so the game plays by the original's rules.

## Features

| | |
|---|---|
| 🎮 **A playable slice** | Title -> character creation (10 classes) -> Northern Undead Asylum -> Asylum Demon -> crow -> Firelink Shrine |
| ⚔️ **Souls-style combat** | stamina, light/heavy/running/plunging attacks, shield block and repel, parry + riposte, backstab, roll i-frames, poise, lock-on camera |
| 🧠 **Original enemy AI, as is** | the game's own Lua 5.0 AI bytecode runs on a tiny Lua VM + a C goal host (`src/ai`); the PSP build prints the same decisions as the PC prototype |
| 🦴 **GE hardware skinning** | 30-bone skeletons, 8-bone chunks, VFPU animation sampling, root motion, cross-fades |
| 🗺️ **Zones with loads** | world zones packed to <= 9 MB, real Havok-derived collision, map event scripts (doors, fog gates, loot, bosses) interpreted at run time |
| 🧰 **Menus and progression** | title, pause, inventory with icons, equipment, status, bonfire menu, Estus, souls, bloodstain, death/respawn, A/B save slots |
| 🩺 **Built for debugging on real hardware** | black-box `log.txt`, kernel-helper `crash.txt` with registers and stack, hang watchdog, GE-hang recovery, `tools/crash_decode.py` |
| 🧪 **Test without a PSP** | PPSSPPHeadless autopilot scenarios, native (x86) runs of the same simulation with an alignment sanitizer, PSP cost-model estimator |

## Quick start

### 1. Build the engine (no game files needed)

Requirements: Linux x86-64, [**pspdev** v20261001](https://github.com/pspdev/pspdev/releases) (GCC 15.2), `cmake`, `ninja`, `curl`, a host C compiler.

```bash
git clone https://github.com/meshoklv/ash && cd ash
export PSPDEV=$HOME/pspdev            # wherever you unpacked pspdev
. scripts/env.sh
tools/ai/build_lua50.sh               # fetches stock Lua 5.0.3 from lua.org (sha256-checked), patches one typedef
psp-cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4               # -> 17 EBOOT.PBP files, e.g. build/src/game/ds/EBOOT.PBP
```

Verified in this release pass: a full build of every target took **79 s** on 4 jobs and finished with 0 errors (15 compiler warnings).

<details>
<summary>Docker instead of a local pspdev (not verified)</summary>

The official `pspdev/pspdev` image should work the same way; mount the repo and the Lua sources, then run the same `psp-cmake` / `cmake --build` lines inside it. We have not run this path.

</details>

### 2. Cook your game data

The converters live in `tools/` (Python 3.13 + [Soulstruct](https://github.com/Grimrukh/soulstruct), installed from git; none of it ships here). Point them at your Dark Souls: Remastered install:

```bash
uv venv -p 3.13 tools/.venv && VIRTUAL_ENV=tools/.venv uv pip install \
  "soulstruct @ git+https://github.com/Grimrukh/soulstruct" "soulstruct-havok @ git+https://github.com/Grimrukh/soulstruct-havok" \
  pillow numpy pyfqmr colorama
export ASH_DSR="/path/to/DARK SOULS REMASTERED"     # your own copy
tools/.venv/bin/python tools/cook_world.py   --out ~/ash-assets/cooked/world/zone_m18_01.pak
tools/.venv/bin/python tools/cook_knight.py  --out-dir ~/ash-assets/cooked
# ... one tool per data type: cook_combat, cook_boss, cook_enemy, cook_events, cook_ui, cook_items, cook_firelink, cook_menu ...
```

Each `tools/cook_*.py` documents its own usage in its header; the file list the game expects is in [`scripts/make-dist.sh`](scripts/make-dist.sh) and [`docs/pipeline.md`](docs/pipeline.md).

> [!WARNING]
> **Not re-run in this pass.** The cookers need a real game copy, which this release pass did not touch, so the cooking commands above come from the tool headers and were not re-verified here. There is also no single "cook everything" script yet.

### 3. Put it on a PSP

```bash
scripts/make-dist.sh              # -> dist/PSP/GAME/{ASHGAME,ASHBENCH,ASHMEM64,ASHVIEW} + dist/ash-psp-<date>-<sha>.zip
```

Unpack the zip onto the root of a Memory Stick, then **XMB -> Game -> Memory Stick -> "Ash: Dark Souls"**. `dist/` contains converted game data: keep it to yourself.

| Requirements on the PSP | |
|---|---|
| Firmware | custom firmware (the EBOOT is an unsigned ELF); tested on **6.60 PRO-C** |
| Model | tested on **PSP Street E1000**; built for the 64 MB mode (`MEMSIZE=1`), peak heap also fits in 24 MB |

**Controls:** stick move · ✕ roll (hold = run) · □ attack · △ heavy · R shield · R+△ parry · L lock-on · ○ Estus · D-pad up = action (doors, items, bonfire) · START menu. After a crash: copy `log.txt` / `crash.txt` off the stick *before* restarting, then `tools/crash_decode.py`.

### Try it without a PSP

`scripts/shoot-ds.sh` drives the game with a scripted autopilot in PPSSPPHeadless (software GPU) and saves frames and a summary log; `tests/native/run.sh route` runs the same simulation natively on x86. Emulator FPS is **not** PSP FPS - the only performance numbers in this README are the hardware ones above. Build recipe for PPSSPPHeadless: [`docs/build.md`](docs/build.md).

## How it works

```mermaid
flowchart LR
  A[(Your Dark Souls:<br>Remastered files)] --> B[tools/cook_*.py<br>Python + Soulstruct]
  B -->|simplify, bake, quantize| C[(cooked .ash* data<br>stays on your disk)]
  D[src/ C99 engine<br>sceGu, VFPU] --> E[EBOOT.PBP]
  C --> F[Memory Stick<br>PSP/GAME/ASHGAME]
  E --> F
  F --> G{{PSP with CFW}}
  G -. log.txt / crash.txt .-> H[tools/crash_decode.py]
```

- **Offline cooking** turns the original formats into compact PSP-friendly ones: meshes simplified to fit a ~20k-triangle frame, lightmaps baked to vertex colour, 8-bit paletted swizzled textures, skeletons cut to 30 bones, animation as 30 Hz quaternion tracks.
- **The engine** is a set of small C99 modules with a fixed 1/30 s simulation step: `world`, `render`, `combat`, `ai`, `boss`, `event`, `items`, `progress`, `menu`, `ui`, `save`, `diag`. Simulation modules have no PSP headers, so they also run natively for tests.
- **Enemy AI** is the original Lua bytecode, repacked to 32-bit floats, on a stock Lua 5.0.3 VM with a hand-written C host.
- **Diagnostics** were built after the first hardware run showed that the emulator hides real bugs (a VRAM address that wrote into kernel memory, a save-utility state that never happens in PPSSPP): see [`docs/hw-pitfalls.md`](docs/hw-pitfalls.md).

Design notes for every module are in [`docs/`](docs/README.md) (written in Russian).

## Configuration

| Variable | Default | Purpose |
|---|---|---|
| `PSPDEV` | `$HOME/opt/pspdev` | pspdev install (`scripts/env.sh`) |
| `ASH_DSR` | `~/ash-assets/dsr` | your Dark Souls: Remastered files, read-only, used by the cookers |
| `ASH_COOKED` | `~/ash-assets/cooked` | where cooked data goes / is read from |
| `ASH_AI`, `ASH_AI_OUT` | `~/ash-assets/ai` | extracted + repacked enemy AI bytecode |
| `LUA50_SRC` (CMake) | `~/opt/lua50/src` | Lua 5.0.3 sources made by `tools/ai/build_lua50.sh` |
| `PPSSPP_HEADLESS` | `~/opt/ppsspp/PPSSPPHeadless` | emulator for the test scripts only |

`~/ash-assets` is just a local working folder name; keep it outside the repository (the `.gitignore` already blocks every game-derived file type).

## Status

Honest picture, from the code and the hardware log:

- [x] **Runs on a real PSP**: boots, plays 26 min, saves, loads, kills the first boss, changes zone (Asylum -> Firelink), no crash.
- [x] Combat, enemy AI, events, items, menus, save/continue, diagnostics.
- [x] Whole first area (Northern Undead Asylum) and Firelink Shrine (bonfire, the first NPC's lines, loot, locked exits).
- [ ] **Performance is not there yet**: the Asylum runs at a ~24 FPS median (15 FPS low end) against a 30 FPS target; a speed pass is in the tree, its effect on hardware is not yet measured.
- [ ] No sound. Several animations and NPCs are placeholders (e.g. one NPC has no animation).
- [ ] Known issues (from the last pre-hardware playtest, not re-checked): after "Continue" the player stands ~2 m from the saved bonfire; a few inventory strings are clipped.
- [ ] Areas beyond Firelink (Undead Burg, Taurus Demon, Parish) are not in this release.
- [ ] One-shot cook script and CI.

**Tested on:** PSP Street E1000, CFW 6.60 PRO-C, 64 MB mode; Linux x86-64 build host; PPSSPP master (headless). **Not tested on:** PSP-1000/2000/3000/Go, ARK-4/5, macOS or Windows build hosts, Docker build.

## FAQ

**Can I play it without owning Dark Souls?** No, and nothing here helps with that; there are no assets in the repository.
**Which game version?** The tools target the PC *Dark Souls: Remastered* file layout.
**Will it be a full port?** No. It is a deliberately cut-down slice (see Status); a handheld with 64 MB cannot hold the game.
**Why do the docs speak Russian?** They are the author's working notes; the code and this README are English.

## Security

The engine opens no sockets. The only network-facing thing in the tree is the *optional* emulator debugger of PPSSPP: never start it with `--debugger=<port>` - it opens a listener.

## Contributing

Issues and pull requests are welcome - see [CONTRIBUTING.md](CONTRIBUTING.md). Never attach game files, cooked data or screenshots of them.

## License

[MIT](LICENSE) for all code in this repository. Third-party components used at build time are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Made by [@meshoklv](https://github.com/meshoklv)
