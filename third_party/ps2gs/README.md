# ps2gs

The Graphics Synthesizer renderer from [ps2-web-recomp](https://github.com/snuri00/ps2-web-recomp)
(MIT, see LICENSE; upstream commit in UPSTREAM_COMMIT): a GIF parser, the GS register model and a
WebGPU (wgpu-native, Metal on Apple platforms) compute-shader rasterizer.

hp keeps its own GS front end (privileged registers, SIGNAL/FINISH/LABEL, interrupts) and uses this
code only to draw, selected with `HP_GS=gpu` (runtime/gs_bridge.cpp).

Local changes: `src/hw.hpp` and `include/ps2/cpu.hpp` are shims for hp; the shaders are embedded by
the Makefile instead of cmake/embed.cmake; `gpu/gpu.cpp` counts image uploads and batch flushes
(`g_hp_*`) for hp's speed report.
