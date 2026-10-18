# Third-party material

This project as a whole is distributed under GPL-3.0-or-later. The pieces below
were ported from or follow other projects.

| Local file | What | Origin project | Upstream file | Upstream license |
| --- | --- | --- | --- | --- |
| `runtime/ipu.c` | IPU command semantics (IDEC, BDEC, VDEC, CSC, ...) and integer IDCT | PCSX2 | `pcsx2/IPU/IPU.cpp`, `pcsx2/IPU/IPU_Fifo.cpp`, `pcsx2/IPU/mpeg2lib/Mpeg.cpp`, `pcsx2/IPU/mpeg2lib/Idct.cpp` | GPL-3.0-or-later |
| `runtime/ipu.c` | MPEG-2 VLC tables (macroblock types, motion codes, DCT coefficients, etc.), following the MPEG-2 standard (ISO/IEC 13818-2) | FFmpeg | `libavcodec/mpeg12data.c` | LGPL-2.1-or-later |
| `runtime/gs.c` | VRAM block and column swizzle tables for the GS memory layout | PCSX2 | `pcsx2/GS/GSTables.cpp` | GPL-3.0-or-later |

Notes:

- The LGPL-2.1-or-later FFmpeg tables may be combined into a GPL-3.0-or-later
  work.
- Hardware behavior (R5900, VU, GS, IOP) was implemented from public
  documentation and observation of the game; no other code was copied.
- No game code or assets are included in this repository.
