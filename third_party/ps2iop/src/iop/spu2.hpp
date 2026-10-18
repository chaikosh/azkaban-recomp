#pragma once

#include <cstdint>
#include <functional>

namespace ps2::state {
class Stream;
}

namespace ps2::iop::spu2 {

void state(ps2::state::Stream &s);

void reset();
std::uint16_t *ram();
std::uint32_t read(std::uint32_t a, unsigned size);
void write(std::uint32_t a, std::uint32_t v, unsigned size);
void dma(int core, std::uint32_t madr, std::uint32_t words, bool to_spu, int channel);
void dump_audio(const char *path);
void dump_voices();
void set_sink(std::function<void(const std::int16_t *samples, std::size_t frames)> sink);

}
