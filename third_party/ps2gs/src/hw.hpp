#pragma once
// Shim for the hp runtime: the GS interrupt is raised by hp's own GS front end, so the
// vendored GS (used only as a renderer) must not raise it a second time.
namespace ps2::hw {
inline void intc_raise(int) {}
}
