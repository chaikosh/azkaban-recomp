# PS2 static recompilation port of SLUS-20926

A static recompiler and host runtime that turn the PlayStation 2 executable of
*Harry Potter and the Prisoner of Azkaban* (SLUS-20926) into a native program.
The long-term goal is a native iOS/macOS build that needs no JIT.

This is an unofficial fan project. It is not affiliated with, endorsed by, or
connected to Electronic Arts or Warner Bros. The repository contains no game
assets and no game code. You must supply your own legally obtained disc.

## Status (October 2026)

Early work in progress.

- Boots, runs the Emotion Engine kernel and the IOP services.
- Shows the loading and memory-card screens correctly.
- Decodes the intro movies (they are not displayed yet).
- Loads the front-end packages.
- No menu and no gameplay yet.
- Sound is stubbed: there is no audio output.
- macOS only so far (SDL3 window). The Metal renderer for iOS is not written.

## How it works

1. `recomp/` is a static recompiler for the PS2 Emotion Engine (MIPS R5900).
   - `r5900.py` decodes instructions, `elf.py` loads the ELF.
   - `analyze.py` finds functions and jump tables. Seeds are `jal` targets,
     data pointers, `lui`/`addiu` constants and prologues; jump tables are
     recovered by constant propagation.
   - `lift.py` emits C: one C function per guest function, labels and `goto`,
     delay slots, likely branches, VU0 macro ops and MMI.
   - `main.py` drives the pipeline.
   - `recomp/hooks.txt` maps guest function addresses to host overrides
     (`hle_*` functions in `runtime/hle.c`).
2. `runtime/` is the host runtime that the generated C is linked against: guest
   memory, the kernel and IOP high-level emulation, DMA, a software GS
   rasterizer, VIF1/VU1, the MPEG-2 IPU, and an SDL3 front end.

The generated C is derived from your own copy of the executable. It is written
to `build/` and is never distributed.

## Build and run

Requirements: macOS, clang, Python 3, and SDL3 (`brew install sdl3`). The GPU
renderer links wgpu-native; the Makefile downloads its static library with
`scripts/fetch_wgpu.sh` on the first build.

You need the contents of your own disc in two places (both git-ignored):

- `disc/`: extracted files (`SLUS_209.26`, `IOP/*.IRX`, `BIG/*.BIG`,
  `FILESYS.DAT`, `RUNTIME.CFG`, `SYSTEM.CNF`).
- `iso/`: the full ISO image (the CD/DVD reads are served from it; the path
  can be overridden with `HP_ISO`).

```sh
make -j8                                                  # recompiles if needed, builds build/hp
# or step by step:
python3 -I recomp/main.py disc/SLUS_209.26 build/recomp   # regenerate the C
./build/hp build/recomp/image.bin disc                    # run
make -j8 RELEASE=1 && HP_GS=gpu ./build/rel/hp build/recomp/image.bin disc   # optimized, GPU renderer
```

`HP_GS=gpu` draws with WebGPU compute shaders (Metal) instead of the software
rasterizer in `runtime/gs.c`; the software renderer stays the default and the
reference.

### Fast test runs

A normal boot (memory card check, EA/WB logos, intro movie, title, menu, New
Game, the train-compartment cutscene) reaches the first 3D gameplay (the train
corridor) after about two minutes. `HP_FASTBOOT=1` gets there in about 10
seconds by using the game's own debug paths (`runtime/fastboot.c`):

- the debug options `-startChapter=Intro -startRoom=HogwartsExpress` are added
  to the options the game reads from `RUNTIME.CFG`, so it skips the logos, the
  movies and the front end and starts the chapter in that room;
- cutscenes are made skippable and Cross is pressed while one plays, so the
  game's own skip ends it (the corridor that follows is the same as after
  watching the scene).

```sh
# corridor in ~10 s; the run ends at vsync 1200 (~20 s)
HP_FASTBOOT=1 HP_EXITAT=1200 HP_GS=gpu HP_SHOT=/tmp/shots HP_SHOTEVERY=60 \
  perl -e 'alarm shift; exec @ARGV' 60 build/rel/hp build/recomp/image.bin disc
```

The log shows `[fastboot 8.7s v518] cutscene over: gameplay` when the corridor
starts (wall seconds since launch, vsync). Other modes and options:

- `HP_FASTBOOT=noskip`: the same start, but cutscenes play (corridor at ~40 s).
- `HP_FASTBOOT=menu`: the normal front end without the FMV (`-nomovie`): the
  main menu at ~22 s instead of ~75 s. Add `HP_AUTOKEYV=1 HP_AUTOKEY=cross@23`
  for New Game (corridor at ~29 s, cutscene skipped).
- `HP_ARGS="-startChapter=Day1 -startRoom=GreatHall"`: start in another room
  (the Great Hall at ~7 s), or pass any other game option; `HP_ARGS` wins over
  the preset. Chapters and rooms are named like the script binaries in
  `MISC.BIG` (`<chapter>_<room>.hpb`: `day1_greathall`, `night2_lowerdungeon`,
  ...). An unknown chapter falls back to the normal front end.

The direct start keeps no memory card profile. Saving a game in the train is
possible from the in-game options page when the level script allows it, but a
save and Load Game path is not needed for test runs: the front end alone takes
longer than the direct start.

## Debugging knobs

Everything is controlled by environment variables read by the runtime. See the
named source file for exact semantics. Sending `SIGUSR1` to the process writes
a diagnostic dump.

| Variable | Area | Purpose |
| --- | --- | --- |
| `HP_TRACE` | core.c | Guest execution trace level |
| `HP_STATS` | hw.c | Periodic DMA statistics |
| `HP_SHOT`, `HP_SHOTFB` | display.c | Save frame / framebuffer screenshots |
| `HP_VRAMDUMP`, `HP_RAMDUMP`, `HP_PEEK` | core.c | Dump VRAM or RAM; `HP_PEEK=addr:len,...` (hex) |
| `HP_WATCH`, `HP_WATCHRING` | core.c | Watch a guest address for writes |
| `HP_CALLTRACE`, `HP_CALLHIST` | core.c | Call tracing / call history |
| `HP_PCRING` | kernel.c | Ring buffer of recent program counters |
| `HP_PRIMLOG`, `HP_PRIMSKIP`, `HP_PRIMAFTER` | gs.c | Log GS primitives |
| `HP_XFERLOG`, `HP_XFERAFTER` | gs.c | Log GS image transfers (with the RAM address of the packet), from a vsync on |
| `HP_TEXDUMP`, `HP_TEXDUMPAT` | gs.c | Write each distinct texture the primitives sample (from a vsync on) as a PPM, with its last upload |
| `HP_TBP0LOG` | gs.c | Report indexed TEX0 setups with TBP0 = 0 (streamed mip levels); with `HP_VUSNAP`/`HP_RAMDUMP` also save the VU1 run / RAM |
| `HP_SYNCLOG` | dma.c | Log VIF1/GIF synchronisation (kicks, I-bit stalls, STC, FLUSHA, completions) for 30 vsyncs |
| `HP_CRTLOG` | dma.c | Log changes of the PCRTC registers (PMODE, SMODE2, DISPFB, DISPLAY) from a vsync on |
| `HP_DUMPAT`, `HP_EXITAT` | hw.c | Run the SIGUSR1 dump at given vsyncs; exit at a vsync (bounded runs) |
| `HP_VIFLOG`, `HP_GIFLOG`, `HP_QLOG` | hle.c | Log VIF, GIF and queue activity |
| `HP_AUTOKEY`, `HP_AUTOKEYV` | display.c | Scripted key presses (`cross@12,start@20`; also `circle`, `square`, `triangle`, `select`, `up`/`down`/`left`/`right`, `lone`/`rone` for L1/R1); with `HP_AUTOKEYV=1` the times are emulated seconds (vsyncs / 59.94), so runs repeat on a loaded machine |
| `HP_FASTBOOT` | fastboot.c | Fast test boot: `1` straight into the train corridor with cutscenes skipped, `noskip`, `menu` (see "Fast test runs") |
| `HP_ARGS`, `HP_ARGSLOG` | fastboot.c | Extra game options (`-startChapter=X -startRoom=Y -startDebugPoint=Z -nomovie ...`); log every option lookup |
| `HP_FSLOG` | fastboot.c | Log the GUI's FSCommands and the cutscenes (start / end, with wall seconds) |
| `HP_HEADLESS` | core.c | Run without a window |
| `HP_GS_THREADS` | gs.c | Number of rasterizer worker threads |
| `HP_GS=gpu`, `HP_GSSYNC` | gs_bridge.cpp | GPU renderer; `HP_GSSYNC` waits for each displayed frame instead of showing the newest finished one |
| `HP_CDLAT`, `HP_CDSEEK` | iop_servers.c | Emulated disc read latency / seek time |
| `HP_ISO` | iop_servers.c | Path to the ISO image |

A few more (`HP_CLOCK`, `HP_SNDDUMP`, `HP_VUDUMP`, `HP_VIFTRACE`,
`HP_CTX2LOG`) are used for narrower investigations; grep for `getenv`.

## Layout

```
recomp/    static recompiler (Python): r5900, elf, analyze, lift, main, hooks.txt
runtime/   host runtime (C): core, kernel, hw, dma, iop, iop_servers, gs, vif,
           vu1, ipu, display, hle
tools/     helper scripts: elfinfo.py, vram.py, vudis.py
Makefile   build driver
disc/ iso/ build/   your game files and generated output (git-ignored)
```

## License and legal

Released under the GNU General Public License, version 3 or (at your option)
any later version. See `LICENSE`. Parts of the runtime are ported from PCSX2
(GPL-3.0); see `THIRD_PARTY.md` for the list of borrowed pieces and their
origins.

No copyrighted game data is included. Do not commit `disc/`, `iso/` or
`build/`. All trademarks belong to their respective owners.
