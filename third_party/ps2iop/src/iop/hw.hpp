#pragma once

#include <cstdint>

namespace ps2::state {
class Stream;
}

namespace ps2::iop::hw {

void state(ps2::state::Stream &s);

void reset();
void raise_interrupt(unsigned line);
void set_interrupt_mask(unsigned line, bool enabled);
int next_pending_interrupt();
void acknowledge_interrupt(unsigned line);
void dma_done(unsigned channel);

std::uint32_t read(std::uint32_t a, unsigned size);
void write(std::uint32_t a, std::uint32_t v, unsigned size);

std::uint16_t *spu2_ram();

}
