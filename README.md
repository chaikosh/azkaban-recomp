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
- macOS (SDL3 window) and iOS (see [iOS](#ios); simulator-tested).

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

## iOS

`Makefile.ios` builds a native arm64 app for the iOS Simulator or an iPhone/iPad
from the same generated C (run `make recomp` or a desktop build first). It needs
Xcode; `ios/deps.sh` (run automatically) fetches wgpu-native for iOS and copies
SDL3 for iOS from a sibling port, or builds it from the SDL release source.

On iOS the paths default to the app sandbox (`runtime/ios/ios.c`):

| Data | Location |
| --- | --- |
| Recompiled image | bundled in the app (`image.bin`) |
| Disc image | `Documents/Harry Potter and the Prisoner of Azkaban (USA).iso` (or any `*.iso` there) |
| Memory card | `Documents/memcard0.ps2` |
| Log (stderr) | `Documents/hp.log` (`HP_STDERR=1` keeps stderr) |
| Extracted files | `Documents/disc/`, only read with `HP_IOP=hle` |

`HP_GS` defaults to `gpu`. The app enables iTunes/Finder file sharing: on a Mac,
select the device in Finder, open Files and drag the ISO onto *HP Azkaban*. If
there is no ISO the app says so and waits (Retry). The ISO is never bundled.

The picture is shown at 4:3 in landscape with an on-screen PS2 controller
(`runtime/ios/touchpad.c`: D-pad, left stick, face buttons, L1/L2/R1/R2,
SELECT/START, multi-touch); it hides while a hardware controller is connected
and comes back on a touch.

```sh
# Simulator (an arm64 iOS Simulator must be booted)
make -f Makefile.ios -j4                 # build/ios-iphonesimulator/HPAzkaban.app
make -f Makefile.ios sim-install sim-data sim-run   # SIM=<udid> instead of the booted one
make -f Makefile.ios sim-log             # Documents/hp.log
# game variables go through SIMCTL_CHILD_*, e.g.
SIMCTL_CHILD_HP_AUTOKEY=start@40 make -f Makefile.ios sim-run

# Device
make -f Makefile.ios SDK=iphoneos -j4    # build/ios-iphoneos/HPAzkaban.app
python3 -I ios/sign_device.py            # signs with an existing local development profile
xcrun devicectl list devices
python3 -I ios/install_device.py --device <id> [--iso "iso/Harry Potter and the Prisoner of Azkaban (USA).iso"]
# or sign and run from Xcode:
make -f Makefile.ios SDK=iphoneos lib xcode && open ios/HPAzkaban.xcodeproj
```

Status of the iOS port (paused, October 2026):

- Works in the simulator (iPhone 17 Pro, iOS 26): the app builds and links,
  installs, finds the ISO in Documents, loads the image, opens the disc and the
  memory card, starts the IOP and the GS, and writes `Documents/hp.log`.
- Broken: the GPU renderer aborts within seconds. The simulator's Metal adapter
  falls short of some WebGPU default limits, so wgpu-native creates the device
  with its downlevel limits (4 storage buffers per shader stage; the GS shaders
  bind 7). Pipeline creation fails, and the first `wgpuQueueSubmit` panics.
  Next step: in `third_party/ps2gs/src/gs/gpu/gpu.cpp` `init()`, request
  `min(supported, WebGPU default)` for the limits the GS needs
  (`maxStorageBuffersPerShaderStage` first). On a Mac that requests exactly the
  defaults, so nothing changes there. Untested: `SIMCTL_CHILD_HP_GS=soft`
  should select the software GS instead.
- Not tested yet: the touch overlay and the 4:3 presentation on screen, audio,
  pause/resume in the background, the "no ISO" dialog, the device build
  (`SDK=iphoneos`), signing, and Xcode project generation. The macOS build is
  unchanged; every iOS path is behind `TARGET_OS_IOS`.

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
| `HP_XFERLOG` | gs.c | Log GS image transfers |
| `HP_VIFLOG`, `HP_GIFLOG`, `HP_QLOG` | hle.c | Log VIF, GIF and queue activity |
| `HP_AUTOKEY` | display.c | Scripted key presses |
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
