#pragma once

#include <cstdint>

namespace ps2::state {
class Stream;
}

namespace ps2::gif {

void state(ps2::state::Stream &s);

void reset();
void stats();
void transfer(int path, const std::uint8_t *data, std::uint32_t qwc);
void render(int path, const std::uint8_t *data, std::uint32_t qwc);
void render_reset();
void scan_signals(int path, const std::uint8_t *data, std::uint32_t qwc);
bool busy(int path);
std::uint32_t read(std::uint32_t a);
void write(std::uint32_t a, std::uint32_t v);
// MSKPATH3 from VIF1, reported back to the EE in GIF_STAT.M3P
void set_path3_masked(bool masked);

}
