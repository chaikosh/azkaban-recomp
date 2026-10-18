#pragma once
// Shim for the hp runtime: EE hardware hooks used by the IOP's SIF.
namespace ps2::hw {
void dmac_raise(int channel);
}
