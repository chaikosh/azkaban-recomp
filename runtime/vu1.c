/* VU1 micro mode interpreter (geometry programs uploaded with VIF MPG). */
#include "rt.h"
#include "gs.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "vu1_core.h"

static VU1 vu;
uint8_t *g_vu1_code;   /* 16 KiB micro memory */
uint8_t *g_vu1_data;   /* 16 KiB data memory */
unsigned long long g_vu1_insns;

/* MAC flags of the last FMAC result (per written component) */
static inline void set_mac(const V4 *r, int dest) {
    uint32_t mac = 0;
    for (int k = 0; k < 4; k++) {
        if (!(dest & (8 >> k))) continue;
        int bit = 3 - k;
        float v = r->f[k];
        if (v == 0.0f) mac |= 1u << bit;
        if (r->u[k] >> 31) mac |= 1u << (bit + 4);
    }
    vu.mac = mac;
    uint32_t st = 0;
    if (mac & 0x0f) st |= 1;
    if (mac & 0xf0) st |= 2;
    vu.status = (vu.status & ~0x3fu) | st | ((vu.status | (st << 6)) & 0xfc0);
}

static inline uint8_t *dmem(uint32_t qaddr) { return g_vu1_data + ((qaddr & 0x3ff) << 4); }

/* ---- upper (FMAC) ---------------------------------------------------------- */
static void upper(uint32_t op, V4 *out_fd, int *out_reg, int *out_dest, int *out_acc) {
    int dest = (op >> 21) & 15, ft = (op >> 16) & 31, fs = (op >> 11) & 31, fd = (op >> 6) & 31, bc = op & 3;
    int f = op & 63;
    V4 s = vu.vf[fs], t = vu.vf[ft], r;
    *out_reg = -1; *out_acc = 0; *out_dest = dest;
    if (f < 60) {
        int kind;       /* 0 add 1 sub 2 madd 3 msub 4 max 5 mini 6 mul */
        int src;        /* 0 bc 1 q 2 i 3 vec */
        if (f < 28) { kind = f >> 2; src = 0; }
        else {
            switch (f) {
            case 28: kind = 6; src = 1; break; case 29: kind = 4; src = 2; break; case 30: kind = 6; src = 2; break;
            case 31: kind = 5; src = 2; break; case 32: kind = 0; src = 1; break; case 33: kind = 2; src = 1; break;
            case 34: kind = 0; src = 2; break; case 35: kind = 2; src = 2; break; case 36: kind = 1; src = 1; break;
            case 37: kind = 3; src = 1; break; case 38: kind = 1; src = 2; break; case 39: kind = 3; src = 2; break;
            case 40: kind = 0; src = 3; break; case 41: kind = 2; src = 3; break; case 42: kind = 6; src = 3; break;
            case 43: kind = 4; src = 3; break; case 44: kind = 1; src = 3; break; case 45: kind = 3; src = 3; break;
            case 46: {   /* OPMSUB */
                r.f[0] = vclampf(vu.acc.f[0] - s.f[1] * t.f[2]);
                r.f[1] = vclampf(vu.acc.f[1] - s.f[2] * t.f[0]);
                r.f[2] = vclampf(vu.acc.f[2] - s.f[0] * t.f[1]);
                r.f[3] = 0;
                *out_fd = r; *out_reg = fd; *out_dest = 0xe;
                set_mac(&r, 0xe);
                return;
            }
            case 47: kind = 5; src = 3; break;
            default: return;
            }
        }
        for (int k = 0; k < 4; k++) {
            float b = src == 0 ? t.f[bc] : src == 1 ? vu.q : src == 2 ? vu.i : t.f[k];
            float a = vin(s.f[k]);
            b = vin(b);
            float v;
            switch (kind) {
            case 0: v = a + b; break;
            case 1: v = a - b; break;
            case 2: v = vu.acc.f[k] + a * b; break;
            case 3: v = vu.acc.f[k] - a * b; break;
            case 4: v = a > b ? a : b; r.f[k] = v; continue;
            case 5: v = a < b ? a : b; r.f[k] = v; continue;
            default: v = a * b; break;
            }
            r.f[k] = vclampf(v);
        }
        *out_fd = r; *out_reg = fd;
        if (kind < 4 || kind == 6) set_mac(&r, dest);
        return;
    }
    int code = (((op >> 6) & 31) << 2) | (op & 3);
    if (code < 16 || (code >= 24 && code <= 28) || (code >= 30 && code <= 30) || (code >= 32 && code <= 46 && code != 43)) {
        int kind, src;
        if (code < 16) { kind = code >> 2; src = 0; }               /* ADDA SUBA MADDA MSUBA bc */
        else if (code < 28) { kind = 6; src = 0; }                   /* MULAbc */
        else switch (code) {
            case 28: kind = 6; src = 1; break; case 30: kind = 6; src = 2; break;
            case 32: kind = 0; src = 1; break; case 33: kind = 2; src = 1; break; case 34: kind = 0; src = 2; break;
            case 35: kind = 2; src = 2; break; case 36: kind = 1; src = 1; break; case 37: kind = 3; src = 1; break;
            case 38: kind = 1; src = 2; break; case 39: kind = 3; src = 2; break; case 40: kind = 0; src = 3; break;
            case 41: kind = 2; src = 3; break; case 42: kind = 6; src = 3; break; case 44: kind = 1; src = 3; break;
            case 45: kind = 3; src = 3; break;
            case 46: {   /* OPMULA */
                r.f[0] = vclampf(s.f[1] * t.f[2]);
                r.f[1] = vclampf(s.f[2] * t.f[0]);
                r.f[2] = vclampf(s.f[0] * t.f[1]);
                r.f[3] = vu.acc.f[3];
                *out_fd = r; *out_acc = 1; *out_dest = 0xe;
                set_mac(&r, 0xe);
                return;
            }
            default: return;
        }
        for (int k = 0; k < 4; k++) {
            float b = src == 0 ? t.f[bc] : src == 1 ? vu.q : src == 2 ? vu.i : t.f[k];
            float a = vin(s.f[k]);
            b = vin(b);
            float v;
            switch (kind) {
            case 0: v = a + b; break;
            case 1: v = a - b; break;
            case 2: v = vu.acc.f[k] + a * b; break;
            case 3: v = vu.acc.f[k] - a * b; break;
            default: v = a * b; break;
            }
            r.f[k] = vclampf(v);
        }
        *out_fd = r; *out_acc = 1;
        set_mac(&r, dest);
        return;
    }
    switch (code) {
    case 16: case 17: case 18: case 19: {   /* ITOF */
        static const float sc[4] = {1.0f, 1.0f / 16, 1.0f / 4096, 1.0f / 32768};
        for (int k = 0; k < 4; k++) r.f[k] = (float)s.i[k] * sc[code - 16];
        *out_fd = r; *out_reg = ft;
        return;
    }
    case 20: case 21: case 22: case 23: {   /* FTOI */
        static const float sc[4] = {1.0f, 16.0f, 4096.0f, 32768.0f};
        for (int k = 0; k < 4; k++) {
            float v = s.f[k] * sc[code - 20];
            r.i[k] = v >= 2147483647.0f ? 0x7fffffff : v <= -2147483648.0f ? (int32_t)0x80000000 : (int32_t)v;
        }
        *out_fd = r; *out_reg = ft;
        return;
    }
    case 29:   /* ABS */
        for (int k = 0; k < 4; k++) r.f[k] = fabsf(s.f[k]);
        *out_fd = r; *out_reg = ft;
        return;
    case 31: {   /* CLIPw */
        float w = fabsf(vin(t.f[3]));
        uint32_t c = 0;
        if (s.f[0] > w) c |= 1; if (s.f[0] < -w) c |= 2;
        if (s.f[1] > w) c |= 4; if (s.f[1] < -w) c |= 8;
        if (s.f[2] > w) c |= 16; if (s.f[2] < -w) c |= 32;
        vu.clip = ((vu.clip << 6) | c) & 0xffffff;
        return;
    }
    case 47: return;   /* NOP */
    }
}

/* ---- lower ----------------------------------------------------------------- */
static int collect_jr;   /* HP_VUCOLLECT: record JR/JALR targets for the recompiler */
static void note_jr(uint32_t target);
static int xgkick_pending = -1, xgkick_due = -1;

static uint32_t gif_packet_size(uint32_t qaddr) {
    uint32_t n = 0;
    for (int guard = 0; guard < 4096; guard++) {
        const uint8_t *p = dmem(qaddr + n);
        uint64_t tag;
        memcpy(&tag, p, 8);
        uint32_t nloop = tag & 0x7fff, flg = (tag >> 58) & 3, nreg = (tag >> 60) & 15;
        if (!nreg) nreg = 16;
        n++;
        if (flg == 0) n += nloop * nreg;
        else if (flg == 1) n += (nloop * nreg + 1) / 2;
        else n += nloop;
        if ((tag >> 15) & 1) break;
        if (n >= 1024) break;
    }
    return n;
}

double g_vu1_kick_time;
/* HP_VURECCHECK: kicks are captured while the recompiled and the interpreted run are compared */
static U128 *cap;
static uint32_t cap_n, cap_max;
static int capturing;
void vu1_xgkick(uint32_t qaddr) {
    struct timespec ka, kb; clock_gettime(CLOCK_MONOTONIC, &ka);
    { extern uint32_t g_last_xgkick; g_last_xgkick = qaddr & 0x3ff; }
    uint32_t n = gif_packet_size(qaddr);
    if (n > 1024) n = 1024;   /* a kick never reads more than the 16 KiB of VU1 data memory (addresses wrap) */
    if (capturing) {
        if (cap_n + n > cap_max) { cap_max = (cap_n + n) * 2; cap = realloc(cap, cap_max * sizeof *cap); }
        for (uint32_t k = 0; k < n; k++) memcpy(&cap[cap_n + k], dmem(qaddr + k), 16);
        cap_n += n;
        return;
    }
    U128 buf[1024];
    for (uint32_t k = 0; k < n; k++) memcpy(&buf[k], dmem(qaddr + k), 16);
    gif_feed(0, buf, n);
    clock_gettime(CLOCK_MONOTONIC, &kb);
    g_vu1_kick_time += (kb.tv_sec - ka.tv_sec) + (kb.tv_nsec - ka.tv_nsec) * 1e-9;
}

static float efu_atan(float x) { return atanf(x); }

/* returns: 0 continue, 1 branch taken (target in *target) */
static int lower(uint32_t op, uint32_t pc, uint32_t *target, V4 *lout, int *lreg, int *ldest, int *vi_reg, uint16_t *vi_val) {
    int opc = op >> 25;
    int dest = (op >> 21) & 15, ft = (op >> 16) & 31, fs = (op >> 11) & 31;
    int it = ft & 15, is_ = fs & 15;
    int32_t imm11 = (int32_t)(op << 21) >> 21;
    *lreg = -1; *vi_reg = -1; *ldest = dest;
    uint16_t VI_IS = vu.vi[is_], VI_IT = vu.vi[it];
    if (opc == 0x40) {
        int f = op & 63;
        int id = (op >> 6) & 15;
        switch (f) {
        case 0x30: *vi_reg = id; *vi_val = (uint16_t)(VI_IS + VI_IT); return 0;
        case 0x31: *vi_reg = id; *vi_val = (uint16_t)(VI_IS - VI_IT); return 0;
        case 0x32: { int imm5 = ((int32_t)(op << 21) >> 27); *vi_reg = it; *vi_val = (uint16_t)(VI_IS + imm5); return 0; }
        case 0x34: *vi_reg = id; *vi_val = VI_IS & VI_IT; return 0;
        case 0x35: *vi_reg = id; *vi_val = VI_IS | VI_IT; return 0;
        }
        if (f < 0x3c) return 0;
        int code = (((op >> 6) & 31) << 2) | (op & 3);
        V4 s = vu.vf[fs], t = vu.vf[ft], r;
        int fsf = (op >> 21) & 3, ftf = (op >> 23) & 3;
        switch (code) {
        case 48: *lout = s; *lreg = ft; return 0;                                          /* MOVE */
        case 49: r.u[0] = s.u[1]; r.u[1] = s.u[2]; r.u[2] = s.u[3]; r.u[3] = s.u[0]; *lout = r; *lreg = ft; return 0;   /* MR32 */
        case 52: memcpy(&r, dmem(VI_IS), 16); *lout = r; *lreg = ft; *vi_reg = is_; *vi_val = (uint16_t)(VI_IS + 1); return 0;   /* LQI */
        case 53: {   /* SQI */
            uint8_t *m = dmem(VI_IT);
            for (int k = 0; k < 4; k++) if (dest & (8 >> k)) memcpy(m + 4 * k, &s.u[k], 4);
            *vi_reg = it; *vi_val = (uint16_t)(VI_IT + 1);
            return 0;
        }
        case 54: { uint16_t a = (uint16_t)(VI_IS - 1); memcpy(&r, dmem(a), 16); *lout = r; *lreg = ft; *vi_reg = is_; *vi_val = a; return 0; }
        case 55: {   /* SQD */
            uint16_t a = (uint16_t)(VI_IT - 1);
            uint8_t *m = dmem(a);
            for (int k = 0; k < 4; k++) if (dest & (8 >> k)) memcpy(m + 4 * k, &s.u[k], 4);
            *vi_reg = it; *vi_val = a;
            return 0;
        }
        case 56:   /* DIV */
            vu_q_start(&vu, vdiv(vin(s.f[fsf]), vin(t.f[ftf])), VU_QLAT_DIV);
            return 0;
        case 57: vu_q_start(&vu, sqrtf(fabsf(vin(t.f[ftf]))), VU_QLAT_DIV); return 0;   /* SQRT */
        case 58: {   /* RSQRT */
            float d = sqrtf(fabsf(vin(t.f[ftf])));
            vu_q_start(&vu, d == 0.0f ? 3.402823e38f : vclampf(vin(s.f[fsf]) / d), VU_QLAT_RSQRT);
            return 0;
        }
        case 59: return 0;   /* WAITQ: the pair waited for Q before it issued (interp) */
        case 60: *vi_reg = it; *vi_val = (uint16_t)s.u[fsf]; return 0;              /* MTIR */
        case 61: for (int k = 0; k < 4; k++) r.i[k] = (int16_t)VI_IS; *lout = r; *lreg = ft; return 0;   /* MFIR */
        case 62: {   /* ILWR */
            const uint8_t *m = dmem(VI_IS);
            int k = dest & 8 ? 0 : dest & 4 ? 1 : dest & 2 ? 2 : 3;
            uint32_t w; memcpy(&w, m + 4 * k, 4);
            *vi_reg = it; *vi_val = (uint16_t)w;
            return 0;
        }
        case 63: {   /* ISWR */
            uint8_t *m = dmem(VI_IS);
            uint32_t w = VI_IT;
            for (int k = 0; k < 4; k++) if (dest & (8 >> k)) memcpy(m + 4 * k, &w, 4);
            return 0;
        }
        case 64: {   /* RNEXT */
            uint32_t x = vu.r; int b = ((x >> 4) ^ (x >> 22)) & 1;
            vu.r = 0x3f800000u | (((x << 1) | b) & 0x7fffff);
            for (int k = 0; k < 4; k++) r.u[k] = vu.r; *lout = r; *lreg = ft; return 0;
        }
        case 65: for (int k = 0; k < 4; k++) r.u[k] = vu.r; *lout = r; *lreg = ft; return 0;   /* RGET */
        case 66: vu.r = 0x3f800000u | (s.u[fsf] & 0x7fffff); return 0;
        case 67: vu.r = 0x3f800000u | ((vu.r ^ s.u[fsf]) & 0x7fffff); return 0;
        case 100: for (int k = 0; k < 4; k++) r.f[k] = vu.p; *lout = r; *lreg = ft; return 0;   /* MFP */
        case 104: *vi_reg = it; *vi_val = (uint16_t)vu.top; return 0;    /* XTOP */
        case 105: *vi_reg = it; *vi_val = (uint16_t)vu.itop; return 0;   /* XITOP */
        case 108: xgkick_pending = VI_IS; return 0;                       /* XGKICK */
        case 112: vu.p = s.f[0] * s.f[0] + s.f[1] * s.f[1] + s.f[2] * s.f[2]; return 0;            /* ESADD */
        case 113: { float d = s.f[0] * s.f[0] + s.f[1] * s.f[1] + s.f[2] * s.f[2]; vu.p = d != 0 ? 1.0f / d : 3.4e38f; return 0; }
        case 114: vu.p = sqrtf(s.f[0] * s.f[0] + s.f[1] * s.f[1] + s.f[2] * s.f[2]); return 0;      /* ELENG */
        case 115: { float d = sqrtf(s.f[0] * s.f[0] + s.f[1] * s.f[1] + s.f[2] * s.f[2]); vu.p = d != 0 ? 1.0f / d : 3.4e38f; return 0; }
        case 116: vu.p = s.f[0] != 0 ? efu_atan(s.f[1] / s.f[0]) : 0; return 0;                    /* EATANxy */
        case 117: vu.p = s.f[0] != 0 ? efu_atan(s.f[2] / s.f[0]) : 0; return 0;                    /* EATANxz */
        case 118: vu.p = s.f[0] + s.f[1] + s.f[2] + s.f[3]; return 0;                              /* ESUM */
        case 120: vu.p = sqrtf(fabsf(s.f[fsf])); return 0;                                         /* ESQRT */
        case 121: { float d = sqrtf(fabsf(s.f[fsf])); vu.p = d != 0 ? 1.0f / d : 3.4e38f; return 0; }   /* ERSQRT */
        case 122: vu.p = s.f[fsf] != 0 ? 1.0f / s.f[fsf] : 3.4e38f; return 0;                       /* ERCPR */
        case 123: return 0;                                                                        /* WAITP */
        case 124: vu.p = sinf(s.f[fsf]); return 0;                                                 /* ESIN */
        case 125: vu.p = efu_atan(s.f[fsf]); return 0;                                             /* EATAN */
        case 126: vu.p = expf(-s.f[fsf]); return 0;                                                /* EEXP */
        }
        return 0;
    }
    switch (opc) {
    case 0x00: {   /* LQ */
        V4 r; memcpy(&r, dmem((uint16_t)(VI_IS + imm11)), 16);
        *lout = r; *lreg = ft; return 0;
    }
    case 0x01: {   /* SQ */
        uint8_t *m = dmem((uint16_t)(VI_IT + imm11));
        V4 s = vu.vf[fs];
        for (int k = 0; k < 4; k++) if (dest & (8 >> k)) memcpy(m + 4 * k, &s.u[k], 4);
        return 0;
    }
    case 0x04: {   /* ILW */
        const uint8_t *m = dmem((uint16_t)(VI_IS + imm11));
        int k = dest & 8 ? 0 : dest & 4 ? 1 : dest & 2 ? 2 : 3;
        uint32_t w; memcpy(&w, m + 4 * k, 4);
        *vi_reg = it; *vi_val = (uint16_t)w; return 0;
    }
    case 0x05: {   /* ISW */
        uint8_t *m = dmem((uint16_t)(VI_IS + imm11));
        uint32_t w = VI_IT;
        for (int k = 0; k < 4; k++) if (dest & (8 >> k)) memcpy(m + 4 * k, &w, 4);
        return 0;
    }
    case 0x08: case 0x09: {   /* IADDIU / ISUBIU */
        uint32_t imm15 = (op & 0x7ff) | ((op >> 10) & 0x7800);
        *vi_reg = it; *vi_val = (uint16_t)(opc == 0x08 ? VI_IS + imm15 : VI_IS - imm15);
        return 0;
    }
    case 0x10: *vi_reg = 1; *vi_val = (vu.clip & 0xffffff) == (op & 0xffffff); return 0;   /* FCEQ */
    case 0x11: vu.clip = op & 0xffffff; return 0;                                         /* FCSET */
    case 0x12: *vi_reg = 1; *vi_val = (vu.clip & op & 0xffffff) != 0; return 0;           /* FCAND */
    case 0x13: *vi_reg = 1; *vi_val = ((vu.clip | op) & 0xffffff) == 0xffffff; return 0;  /* FCOR */
    case 0x14: case 0x15: case 0x16: case 0x17: {   /* FSEQ FSSET FSAND FSOR */
        uint32_t imm12 = (op & 0x7ff) | ((op >> 10) & 0x800);
        switch (opc) {
        case 0x14: *vi_reg = it; *vi_val = (vu.status & 0xfff) == imm12; break;
        case 0x15: vu.status = (vu.status & 0x3f) | (imm12 & 0xfc0); break;
        case 0x16: *vi_reg = it; *vi_val = (uint16_t)(vu.status & imm12); break;
        default: *vi_reg = it; *vi_val = (uint16_t)(vu.status | imm12); break;
        }
        return 0;
    }
    case 0x18: *vi_reg = it; *vi_val = (vu.mac & 0xffff) == VI_IS; return 0;              /* FMEQ */
    case 0x1a: *vi_reg = it; *vi_val = (uint16_t)(vu.mac & VI_IS); return 0;               /* FMAND */
    case 0x1b: *vi_reg = it; *vi_val = (uint16_t)(vu.mac | VI_IS); return 0;               /* FMOR */
    case 0x1c: *vi_reg = it; *vi_val = (uint16_t)(vu.clip & 0xfff); return 0;              /* FCGET */
    case 0x20: *target = pc + 1 + imm11; return 1;                                         /* B */
    case 0x21: *vi_reg = it; *vi_val = (uint16_t)(pc + 2); *target = pc + 1 + imm11; return 1;   /* BAL */
    case 0x24: *target = VI_IS; if (collect_jr) note_jr(VI_IS); return 1;                 /* JR */
    case 0x25: *vi_reg = it; *vi_val = (uint16_t)(pc + 2); *target = VI_IS; if (collect_jr) note_jr(VI_IS); return 1;   /* JALR */
    case 0x28: if (VI_IT == VI_IS) { *target = pc + 1 + imm11; return 1; } return 0;
    case 0x29: if (VI_IT != VI_IS) { *target = pc + 1 + imm11; return 1; } return 0;
    case 0x2c: if ((int16_t)VI_IS < 0) { *target = pc + 1 + imm11; return 1; } return 0;
    case 0x2d: if ((int16_t)VI_IS > 0) { *target = pc + 1 + imm11; return 1; } return 0;
    case 0x2e: if ((int16_t)VI_IS <= 0) { *target = pc + 1 + imm11; return 1; } return 0;
    case 0x2f: if ((int16_t)VI_IS >= 0) { *target = pc + 1 + imm11; return 1; } return 0;
    }
    return 0;
}

static void write_vf(int reg, const V4 *v, int dest) {
    if (reg <= 0) return;
    for (int k = 0; k < 4; k++) if (dest & (8 >> k)) vu.vf[reg].u[k] = v->u[k];
}

/* Run from `start` (instruction pairs) until the E bit; `top`/`itop` from VIF. */
/* HP_VUSNAP=dir: keep the state each run starts from; the first implausible GS write
   (gs.c, HP_GSCHECK) saves the code, the pre-run data and registers of the run that made it */
static uint8_t snap_data[16384];
static uint8_t snap_regs[4096];
static uint32_t snap_start, snap_top, snap_itop;
static int snap_on = -1;
void vu1_snapshot_save(void) {
    static int saved;
    const char *dir = getenv("HP_VUSNAP");
    if (!dir || saved++) return;
    char path[1024]; FILE *f;
    snprintf(path, sizeof path, "%s/code.bin", dir); f = fopen(path, "wb"); if (f) { fwrite(g_vu1_code, 1, 16384, f); fclose(f); }
    snprintf(path, sizeof path, "%s/data.bin", dir); f = fopen(path, "wb"); if (f) { fwrite(snap_data, 1, 16384, f); fclose(f); }
    snprintf(path, sizeof path, "%s/regs.bin", dir); f = fopen(path, "wb"); if (f) { fwrite(snap_regs, 1, sizeof snap_regs, f); fclose(f); }
    LOG("[vu1] snapshot of the run at %04x (top %04x itop %04x) saved to %s\n", snap_start, snap_top, snap_itop, dir);
}
/* HP_VUSTATS: which microprograms run (code image hash, entry); HP_VUCOLLECT=dir also saves each
   code image (code_<hash>.bin) and its entries (entries.txt) for the VU1 recompiler. */
unsigned g_vu1_code_gen = 1, g_vu1_mpg_words;
static unsigned code_gen_seen;
static uint64_t code_hash;
typedef struct { uint64_t hash; uint32_t start; unsigned long long runs, pairs; } VuProg;
static VuProg progs[512];
static int nprogs;
uint64_t vu1_code_hash(void) {
    if (code_gen_seen != g_vu1_code_gen) {
        uint64_t h = 1469598103934665603ull;
        for (int k = 0; k < 16384; k++) { h ^= g_vu1_code[k]; h *= 1099511628211ull; }
        code_hash = h;
        code_gen_seen = g_vu1_code_gen;
    }
    return code_hash;
}
static VuProg *prog_note(uint32_t start) {
    uint64_t h = vu1_code_hash();
    for (int k = 0; k < nprogs; k++) if (progs[k].hash == h && progs[k].start == start) { progs[k].runs++; return &progs[k]; }
    if (nprogs >= 512) return NULL;
    VuProg *p = &progs[nprogs++];
    p->hash = h; p->start = start; p->runs = 1; p->pairs = 0;
    static const char *dir = (const char *)1;
    if (dir == (const char *)1) dir = getenv("HP_VUCOLLECT");
    if (dir) {
        char path[1100];
        snprintf(path, sizeof path, "%s/code_%016llx.bin", dir, (unsigned long long)h);
        FILE *f = fopen(path, "rb");
        if (f) fclose(f);
        else if ((f = fopen(path, "wb"))) { fwrite(g_vu1_code, 1, 16384, f); fclose(f); }
        snprintf(path, sizeof path, "%s/entries.txt", dir);
        if ((f = fopen(path, "a"))) { fprintf(f, "%016llx %04x\n", (unsigned long long)h, start); fclose(f); }
    }
    return p;
}
static void note_jr(uint32_t target) {
    static struct { uint64_t hash; uint8_t seen[2048]; } tab[16];
    static int n;
    uint64_t h = vu1_code_hash();
    int k;
    for (k = 0; k < n && tab[k].hash != h; k++) ;
    if (k == n) { if (n == 16) return; tab[n].hash = h; memset(tab[n].seen, 0, 2048); n++; }
    target &= 0x7ff;
    if (tab[k].seen[target]) return;
    tab[k].seen[target] = 1;
    const char *dir = getenv("HP_VUCOLLECT");
    char path[1100];
    snprintf(path, sizeof path, "%s/entries.txt", dir);
    FILE *f = fopen(path, "a");
    if (f) { fprintf(f, "%016llx %04x jr\n", (unsigned long long)h, target); fclose(f); }
}
void vu1_stats(void) {
    static unsigned last_gen, last_words;
    LOG("[vu1] %d programs seen, code changed %u times, %u MPG words since last report\n", nprogs, g_vu1_code_gen - last_gen, g_vu1_mpg_words - last_words);
    last_gen = g_vu1_code_gen; last_words = g_vu1_mpg_words;
    for (int k = 0; k < nprogs; k++)
        if (progs[k].runs) LOG("[vu1]   %016llx @%04x runs %llu pairs %llu\n", (unsigned long long)progs[k].hash, progs[k].start, progs[k].runs, progs[k].pairs);
    for (int k = 0; k < nprogs; k++) progs[k].runs = progs[k].pairs = 0;
}

/* ---- recompiled microprograms (recomp/vu1rec.py -> build/recomp/vu1_gen.c) ------------------ */
static void interp(int ending, int branch_pending, uint32_t branch_target);
static int rec_on = -1, rec_check;
static unsigned long long rec_runs, rec_bails, rec_misses, rec_bad;
static int rec_enabled(void) {
    if (rec_on < 0) {
        const char *e = getenv("HP_VUREC");
        rec_on = !e || strcmp(e, "0") != 0;
        rec_check = getenv("HP_VURECCHECK") != NULL;
        int n = 0;
        while (g_vu1rec_table[n].fn) n++;
        LOG("[vu1rec] %d recompiled code images%s%s\n", n, rec_on ? "" : " (disabled by HP_VUREC=0)", rec_check ? ", checking every run" : "");
    }
    return rec_on;
}
/* Compiled programs are matched against the pairs they were compiled from, not the whole
   micro memory: the game's uploads leave different leftovers around the same renderer. */
static const VuRecEntry *rec_valid[64];
static int rec_nvalid;
static void rec_revalidate(void) {
    rec_nvalid = 0;
    for (int k = 0; g_vu1rec_table[k].fn && rec_nvalid < 64; k++) {
        const VuRecEntry *e = &g_vu1rec_table[k];
        uint64_t h = 1469598103934665603ull;
        for (const uint16_t *r = e->ranges; *r != 0xffff; r += 2)
            for (uint32_t n = r[0]; n < r[1]; n++)
                for (int b = 0; b < 8; b++) {
                    int byte = g_vu1_code[(n & 0x7ff) * 8 + b];
                    h ^= (uint8_t)byte; h *= 1099511628211ull;
                }
        if (h == e->key) rec_valid[rec_nvalid++] = e;
    }
    /* several can match at once (a small partial image and the full one): try the most complete first */
    int count[64];
    for (int k = 0; k < rec_nvalid; k++) {
        count[k] = 0;
        for (int b = 0; b < 256; b++) count[k] += __builtin_popcount(rec_valid[k]->labels[b]);
    }
    for (int i = 1; i < rec_nvalid; i++)
        for (int j = i; j > 0 && count[j] > count[j - 1]; j--) {
            const VuRecEntry *t = rec_valid[j]; rec_valid[j] = rec_valid[j - 1]; rec_valid[j - 1] = t;
            int c = count[j]; count[j] = count[j - 1]; count[j - 1] = c;
        }
}
static const VuRecEntry *rec_lookup(uint32_t start) {
    static unsigned gen_seen;
    extern unsigned g_vu1_code_gen;
    if (gen_seen != g_vu1_code_gen) { rec_revalidate(); gen_seen = g_vu1_code_gen; }
    for (int k = 0; k < rec_nvalid; k++)
        if (rec_valid[k]->labels[start >> 3] & (1u << (start & 7))) return rec_valid[k];
    static uint32_t missed[64]; static int nmissed;
    uint32_t key = (g_vu1_code_gen << 11) | start;
    int seen = 0;
    for (int k = 0; k < nmissed; k++) seen |= missed[k] == key;
    if (!seen && nmissed < 64) {
        missed[nmissed++] = key;
        extern unsigned long long g_vsyncs;
        LOG("[vu1rec] no recompiled program for %04x in code image %016llx (vsync %llu); collect with HP_VUCOLLECT=build/recomp/vu1\n",
            start, (unsigned long long)vu1_code_hash(), g_vsyncs);
    }
    return NULL;
}
static void rec_run(const VuRecEntry *e, uint32_t start) {
    VuResume r;
    rec_runs++;
    if (!e->fn(&vu, start, &r)) return;
    rec_bails++;
    if (r.jr_miss) {
        rec_misses++;
        static int n;
        if (n++ < 20) LOG("[vu1rec] %016llx: no compiled code at %04x (start %04x), interpreting\n", (unsigned long long)e->hash, r.pc, start);
    }
    vu.pc = r.pc;
    xgkick_due = r.due;
    interp(r.ending, r.branch_pending, r.branch_target);
}
static int same32(const void *a, const void *b) { return !memcmp(a, b, 4); }
static void rec_run_checked(const VuRecEntry *e, uint32_t start) {
    static uint8_t data0[16384], data1[16384];
    static U128 *kicks1;
    static uint32_t kicks1_max;
    VU1 vu0 = vu;
    memcpy(data0, g_vu1_data, 16384);
    capturing = 1; cap_n = 0;
    rec_run(e, start);
    VU1 vu1 = vu;
    memcpy(data1, g_vu1_data, 16384);
    uint32_t n1 = cap_n;
    if (n1 > kicks1_max) { kicks1_max = n1 * 2; kicks1 = realloc(kicks1, kicks1_max * sizeof *kicks1); }
    if (n1) memcpy(kicks1, cap, n1 * sizeof *cap);
    vu = vu0;
    memcpy(g_vu1_data, data0, 16384);
    xgkick_due = -1;
    cap_n = 0;
    interp(0, 0, 0);
    capturing = 0;
    char what[256] = "";
    for (int k = 1; k < 32 && !what[0]; k++)
        for (int c = 0; c < 4; c++)
            if (vu1.vf[k].u[c] != vu.vf[k].u[c]) { snprintf(what, sizeof what, "vf%d.%c %08x vs %08x", k, "xyzw"[c], vu1.vf[k].u[c], vu.vf[k].u[c]); break; }
    for (int c = 0; c < 4 && !what[0]; c++)
        if (vu1.acc.u[c] != vu.acc.u[c]) snprintf(what, sizeof what, "acc.%c %08x vs %08x", "xyzw"[c], vu1.acc.u[c], vu.acc.u[c]);
    for (int k = 1; k < 16 && !what[0]; k++)
        if (vu1.vi[k] != vu.vi[k]) snprintf(what, sizeof what, "vi%d %04x vs %04x", k, vu1.vi[k], vu.vi[k]);
    if (!what[0] && !same32(&vu1.q, &vu.q)) snprintf(what, sizeof what, "Q");
    if (!what[0] && !same32(&vu1.p, &vu.p)) snprintf(what, sizeof what, "P");
    if (!what[0] && !same32(&vu1.i, &vu.i)) snprintf(what, sizeof what, "I");
    if (!what[0] && vu1.r != vu.r) snprintf(what, sizeof what, "R");
    if (!what[0] && vu1.clip != vu.clip) snprintf(what, sizeof what, "clip %06x vs %06x", vu1.clip, vu.clip);
    if (!what[0] && vu1.pc != vu.pc) snprintf(what, sizeof what, "tpc %04x vs %04x", vu1.pc, vu.pc);
    /* MAC/status are not compared: recompiled code computes them only where a later instruction
       reads them, and any such read shows up in VI registers or memory */
    if (!what[0] && memcmp(data1, g_vu1_data, 16384)) {
        int k = 0; while (data1[k] == g_vu1_data[k]) k++;
        snprintf(what, sizeof what, "data memory at qword %03x", k / 16);
    }
    if (!what[0] && (n1 != cap_n || memcmp(kicks1, cap, n1 * sizeof *cap))) snprintf(what, sizeof what, "kicks: %u vs %u qwords", n1, cap_n);
    if (what[0]) {
        rec_bad++;
        static int n;
        if (n++ < 20) LOG("[vu1rec] MISMATCH %016llx start %04x: %s\n", (unsigned long long)e->hash, start, what);
    }
    if (cap_n) gif_feed(0, cap, cap_n);   /* the interpreter's result is the one that counts */
}
void vu1rec_stats(void) {
    LOG("[vu1rec] runs %llu, handed to the interpreter %llu (unknown jump targets %llu), check mismatches %llu\n", rec_runs, rec_bails, rec_misses, rec_bad);
}

/* TPC: where the last microprogram stopped (the pair after its E-bit delay slot); MSCNT resumes here */
uint32_t vu1_tpc(void) { return vu.pc; }
void vu1_run(uint32_t start, uint32_t top, uint32_t itop) {
    { extern uint32_t g_last_vu1_start; g_last_vu1_start = start; }
    if (snap_on < 0) snap_on = getenv("HP_VUSNAP") != NULL;
    if (snap_on) {
        memcpy(snap_data, g_vu1_data, 16384);
        memcpy(snap_regs, &vu, sizeof vu < sizeof snap_regs ? sizeof vu : sizeof snap_regs);
        snap_start = start; snap_top = top; snap_itop = itop;
    }
    static int dumped;
    const char *dump = getenv("HP_VUDUMP");
    if (dump && dumped++ == 20) {
        char path[1024];
        snprintf(path, sizeof path, "%s/vu1_code.bin", dump);
        FILE *f = fopen(path, "wb"); if (f) { fwrite(g_vu1_code, 1, 16384, f); fclose(f); }
        snprintf(path, sizeof path, "%s/vu1_data.bin", dump);
        f = fopen(path, "wb"); if (f) { fwrite(g_vu1_data, 1, 16384, f); fclose(f); }
        LOG("[vu1] dumped micro/data memory at run start %04x top %04x itop %04x\n", start, top, itop);
    }
    static int stats_on = -1;
    if (stats_on < 0) { stats_on = getenv("HP_VUSTATS") || getenv("HP_VUCOLLECT"); collect_jr = getenv("HP_VUCOLLECT") != NULL; }
    VuProg *prog = stats_on ? prog_note(start & 0x7ff) : NULL;
    unsigned long long insns0 = g_vu1_insns;
    vu.pc = start & 0x7ff;
    vu.top = top; vu.itop = itop;
    vu.vf[0].f[0] = vu.vf[0].f[1] = vu.vf[0].f[2] = 0; vu.vf[0].f[3] = 1.0f;
    if (rec_enabled()) {
        const VuRecEntry *e = rec_lookup(start & 0x7ff);
        if (e) {
            if (rec_check) rec_run_checked(e, start & 0x7ff);
            else rec_run(e, start & 0x7ff);
            if (prog) prog->pairs += g_vu1_insns - insns0;
            return;
        }
    }
    interp(0, 0, 0);
    if (prog) prog->pairs += g_vu1_insns - insns0;
}

/* The interpreter loop, from vu.pc with the given sequencing state (a fresh run, or where a
   recompiled program handed over). */
static void interp(int ending, int branch_pending, uint32_t branch_target) {
    for (long steps = 0; steps < 5000000; steps++) {
        uint32_t pc = vu.pc;
        uint64_t pair;
        memcpy(&pair, g_vu1_code + (pc & 0x7ff) * 8, 8);
        uint32_t lo = (uint32_t)pair, up = (uint32_t)(pair >> 32);
        g_vu1_insns++;
        V4 ures; int ureg, udest, uacc;
        V4 lres; int lreg, ldest, vireg = -1; uint16_t vival = 0;
        uint32_t tgt = 0;
        int br = 0;
        xgkick_pending = -1;
        if (!(up & 0x80000000u) && vu_lower_waits_q(lo)) vu_q_flush(&vu);   /* WAITQ / busy FDIV stall the pair */
        upper(up, &ures, &ureg, &udest, &uacc);
        if (!(up & 0x80000000u)) br = lower(lo, pc, &tgt, &lres, &lreg, &ldest, &vireg, &vival);
        else { lreg = -1; }
        /* commit: lower results first so the upper wins on a conflict */
        if (lreg >= 0) write_vf(lreg, &lres, ldest);
        if (ureg >= 0) write_vf(ureg, &ures, udest);
        if (uacc) for (int k = 0; k < 4; k++) if (udest & (8 >> k)) vu.acc.u[k] = ures.u[k];
        if (vireg > 0) vu.vi[vireg] = vival;
        if (up & 0x80000000u) { union { uint32_t u; float f; } c = {lo}; vu.i = c.f; }
        vu_q_tick(&vu);
        /* XGKICK: the GIF starts reading the packet a little later, so the pair after the kick can
           still finish its tag (games write NLOOP right after XGKICK); kick one pair late */
        if (xgkick_due >= 0) { vu1_xgkick((uint32_t)xgkick_due); xgkick_due = -1; }
        if (xgkick_pending >= 0) xgkick_due = xgkick_pending;
        /* sequencing: delay slots for branches and the E bit */
        uint32_t next = pc + 1;
        if (branch_pending) { next = branch_target; branch_pending = 0; }
        if (br) { branch_pending = 1; branch_target = tgt; }
        vu.pc = next & 0x7ff;
        if (ending) {
            if (xgkick_due >= 0) { vu1_xgkick((uint32_t)xgkick_due); xgkick_due = -1; }
            vu_q_flush(&vu);   /* the FDIV finishes after the program; the next one sees its Q */
            return;
        }
        if (up & 0x40000000u) ending = 1;   /* E bit: one more pair runs */
    }
    LOG("[vu1] program did not finish (pc %04x)\n", vu.pc);
}

void vu1_init(uint8_t *vumem_page) {
    g_vu1_code = vumem_page + 0x8000;
    g_vu1_data = vumem_page + 0xc000;
}
