/* VU0 control registers seen from the EE (cfc2/ctc2). */
#include "rt.h"

static uint32_t vu0_fbrst, vu0_cmsar0, vu1_cmsar1;

uint32_t vu0_cfc2(EE *c, int reg) {
    if (reg < 16) return c->vi[reg];
    switch (reg) {
    case 16: return c->vstatus;
    case 17: return c->vmac;
    case 18: return c->vclip;
    case 20: return c->vr;
    case 21: { FPR f; f.f = c->vi_i; return f.u; }
    case 22: { FPR f; f.f = c->vq; return f.u; }
    case 26: return 0;               /* TPC */
    case 27: return vu0_cmsar0;
    case 28: return vu0_fbrst;
    case 29: return 0;               /* VPU-STAT: VU0 and VU1 idle */
    case 31: return vu1_cmsar1;
    }
    return 0;
}

void vu1_start(uint32_t addr);

void vu0_ctc2(EE *c, int reg, uint32_t v) {
    if (reg > 0 && reg < 16) { c->vi[reg] = (uint16_t)v; return; }
    switch (reg) {
    case 16: c->vstatus = v & 0xfc0; return;
    case 18: c->vclip = v & 0xffffff; return;
    case 20: c->vr = v; return;
    case 21: { FPR f; f.u = v; c->vi_i = f.f; return; }
    case 22: { FPR f; f.u = v; c->vq = f.f; return; }
    case 27: vu0_cmsar0 = v; LOG("[vu0] CMSAR0 = %04x (micro mode not supported)\n", v); return;
    case 28: vu0_fbrst = v & 0x0a0a; return;
    case 31: vu1_cmsar1 = v; vu1_start(v); return;
    }
}

void vu1_start(uint32_t addr) {
    static int n;
    if (n++ < 5) LOG("[vu1] start at %04x via CMSAR1\n", addr);
}
