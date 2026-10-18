#pragma once
// Shim for the hp runtime: the parts of ps2-web-recomp's system interface the IOP code uses.
#include <cstdint>
#include <functional>
#include <string>

namespace ps2 {

inline constexpr std::uint64_t kEeClock = 294912000ull;

struct Options {
    bool trace_iop_modules = false;
    bool trace_iop_hle = false;
    std::string memcard_path;
    std::function<void()> on_memcard_saved;
};

extern Options g_options;

std::uint64_t now();   // EE cycles

enum class Rendered { idle, presented, busy };   // used by the vendored GS
std::uint64_t frame_count();                      // vblanks so far (GPU profiling)

}
