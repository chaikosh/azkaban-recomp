#pragma once

#include "gs.hpp"

namespace ps2::gs::raster {

void reset();
void draw(const State &s, const Vertex *v, int count);

}
