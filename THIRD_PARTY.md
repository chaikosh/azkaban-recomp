# Third-party material

This project as a whole is distributed under GPL-3.0-or-later. The pieces below
were ported from or follow other projects.

| Local file | What | Origin project | Upstream file | Upstream license |
| --- | --- | --- | --- | --- |
| `runtime/ipu.c` | IPU command semantics (IDEC, BDEC, VDEC, CSC, ...) and integer IDCT | PCSX2 | `pcsx2/IPU/IPU.cpp`, `pcsx2/IPU/IPU_Fifo.cpp`, `pcsx2/IPU/mpeg2lib/Mpeg.cpp`, `pcsx2/IPU/mpeg2lib/Idct.cpp` | GPL-3.0-or-later |
| `runtime/ipu.c` | MPEG-2 VLC tables (macroblock types, motion codes, DCT coefficients, etc.), following the MPEG-2 standard (ISO/IEC 13818-2) | FFmpeg | `libavcodec/mpeg12data.c` | LGPL-2.1-or-later |
| `runtime/gs.c` | VRAM block and column swizzle tables for the GS memory layout | PCSX2 | `pcsx2/GS/GSTables.cpp` | GPL-3.0-or-later |
| `third_party/ps2iop/` (IOP R3000 interpreter, IOP kernel and library HLE, SIF, SPU2, SIO2, CDVD) and its bridge `runtime/iop_bridge.cpp` | Low-level IOP: runs the game's own IOP modules (sound driver, streaming, pads, memory cards) | ps2-web-recomp | `runtime/src/iop/*`, `runtime/src/disc.cpp`, `runtime/include/ps2/{disc,state}.hpp` | MIT (Copyright (c) 2026 Samir Nuri); local changes listed in `third_party/ps2iop/README` |
| `third_party/ps2gs/` (GIF parser, GS register model, WebGPU compute-shader rasterizer and WGSL shaders) and its bridge `runtime/gs_bridge.cpp` | GPU renderer selected with `HP_GS=gpu` | ps2-web-recomp | `runtime/src/gs/*`, `runtime/src/gs/gpu/*` | MIT (Copyright (c) 2026 Samir Nuri); local changes listed in `third_party/ps2gs/README.md` |
| `third_party/wgpu-native/include/` (headers; the static library is downloaded by `scripts/fetch_wgpu.sh`) | WebGPU implementation used by the GPU renderer (Metal on Apple platforms) | wgpu-native | `ffi/webgpu-headers/webgpu.h`, `ffi/wgpu.h` | MIT or Apache-2.0 (wgpu-native), BSD-3-Clause (webgpu-headers) |

Notes:

- The LGPL-2.1-or-later FFmpeg tables may be combined into a GPL-3.0-or-later
  work.
- The MIT-licensed ps2-web-recomp IOP and GS code may be combined into a GPL-3.0-or-later
  work; its license text is kept in `third_party/ps2iop/LICENSE` and
  `third_party/ps2gs/LICENSE`.
- Hardware behavior (R5900, VU, GS, IOP) was implemented from public
  documentation and observation of the game; no other code was copied.
- No game code or assets are included in this repository.
