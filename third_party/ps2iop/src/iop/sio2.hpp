#pragma once

#include <cstdint>

namespace ps2::state {
class Stream;
}

namespace ps2::iop::sio2 {

void state(ps2::state::Stream &s);

void reset();
std::uint32_t read(std::uint32_t a);
void write(std::uint32_t a, std::uint32_t v);
void arm_dma(int channel, std::uint32_t madr, std::uint32_t block_words, std::uint32_t blocks);
void set_buttons(int pad, std::uint16_t pressed_mask);
void set_axis(int pad, int axis, std::uint8_t value);
void set_pressure(int pad, int button, std::uint8_t value);
void set_connected(int pad, bool connected);
std::uint16_t rumble(int pad);
void flush_cards();

}
