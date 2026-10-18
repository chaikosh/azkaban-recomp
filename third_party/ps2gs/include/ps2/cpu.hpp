#pragma once
// Shim for the hp runtime: the 128-bit register type ps2-web-recomp's GS code uses.
#include <cstdint>

namespace ps2 {

union alignas(16) Reg128 {
    std::uint64_t d[2];
    std::int64_t sd[2];
    std::uint32_t w[4];
    std::int32_t sw[4];
    std::uint16_t h[8];
    std::int16_t sh[8];
    std::uint8_t b[16];
    std::int8_t sb[16];
    float f[4];
};

}
