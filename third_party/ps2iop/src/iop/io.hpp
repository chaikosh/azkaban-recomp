#pragma once

#include <cstdint>
#include <string>

namespace ps2::state {
class Stream;
}

namespace ps2::iop::io {

void state(ps2::state::Stream &s);

int open(const std::string &path, std::uint32_t flags);
int close(int fd);
int read(int fd, std::uint8_t *out, std::uint32_t size);
int write(int fd, const std::uint8_t *data, std::uint32_t size);
std::int64_t seek(int fd, std::int64_t offset, int whence);
bool stat(const std::string &path, std::uint32_t &mode, std::uint32_t &size);
void reset();

}
