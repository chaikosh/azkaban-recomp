#pragma once
// Shim for the hp runtime: the EE memory view the vendored IOP code needs (see runtime/iop_bridge.cpp).
#include <cstdint>
#include <cstring>

namespace ps2 {

inline constexpr std::uint32_t kRamSize = 32u << 20;
inline constexpr std::uint32_t kScratchpadBase = 0x70000000u;
inline constexpr std::uint32_t kScratchpadSize = 16u << 10;

extern std::uint8_t g_ram[];   // EE main RAM, shared with the hp runtime
extern std::uint8_t g_spr[];

inline void watch_span(std::uint32_t, std::uint32_t, const char *) {}

template <typename T>
inline T load(const void *p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

template <typename T>
inline void store(void *p, T v) { std::memcpy(p, &v, sizeof v); }

}
