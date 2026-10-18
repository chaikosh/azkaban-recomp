/* VU1 state and the arithmetic helpers shared by the interpreter (vu1.c) and the statically
 * recompiled microprograms (build/recomp/vu1_gen.c, made by recomp/vu1rec.py). */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef union { float f[4]; uint32_t u[4]; int32_t i[4]; } V4;

typedef struct {
    V4 vf[32], acc;
    uint16_t vi[16];
    float q, p, i;
    uint32_t r;
    uint32_t mac, status, clip;
    uint32_t pc;          /* in instruction pairs */
    uint32_t top, itop;
} VU1;

/* Where a recompiled program hands over to the interpreter (a case it does not compile). */
typedef struct {
    uint32_t pc;
    int ending, branch_pending, due, jr_miss;
    uint32_t branch_target;
} VuResume;

/* Recompiled code images (build/recomp/vu1_gen.c): returns 0 when the program finished, 1 when
   the interpreter has to continue from *res. */
typedef int (*VuRecFn)(VU1 *v, uint32_t start, VuResume *res);
typedef struct {
    uint64_t hash;             /* code image it was made from (for reports) */
    VuRecFn fn;
    int flags;
    const uint16_t *ranges;    /* pairs it was compiled from: [a, b) ..., 0xffff */
    uint64_t key;              /* FNV-1a of those pairs: valid while micro memory matches */
    const uint8_t *labels;     /* bitmap of the pairs it can start at */
} VuRecEntry;
extern const VuRecEntry g_vu1rec_table[];

extern uint8_t *g_vu1_data;
void vu1_xgkick(uint32_t qaddr);

static inline float vclampf(float x) {
    union { float f; uint32_t u; } c = {x};
    if ((c.u & 0x7f800000u) == 0x7f800000u) c.u = (c.u & 0x80000000u) | 0x7f7fffffu;
    return c.f;
}
static inline float vin(float x) {   /* VU treats denormals as zero and Inf/NaN as max */
    union { float f; uint32_t u; } c = {x};
    uint32_t e = c.u & 0x7f800000u;
    if (e == 0) c.u &= 0x80000000u;
    else if (e == 0x7f800000u) c.u = (c.u & 0x80000000u) | 0x7f7fffffu;
    return c.f;
}

static inline float vmax(float a, float b) { return a > b ? a : b; }
static inline float vmin(float a, float b) { return a < b ? a : b; }
static inline int32_t vftoi(float v) {
    return v >= 2147483647.0f ? 0x7fffffff : v <= -2147483648.0f ? (int32_t)0x80000000 : (int32_t)v;
}
static inline float vdiv(float a, float b) {   /* a, b already through vin() */
    if (b == 0.0f) { union { float f; uint32_t u; } c = {a}, d = {b}; c.u = ((c.u ^ d.u) & 0x80000000u) | 0x7f7fffffu; return c.f; }
    return vclampf(a / b);
}

/* MAC flags of the last FMAC result (per written component) and the status flags */
static inline void vu_set_mac(VU1 *v, const V4 *r, int dest) {
    uint32_t mac = 0;
    for (int k = 0; k < 4; k++) {
        if (!(dest & (8 >> k))) continue;
        int bit = 3 - k;
        float x = r->f[k];
        if (x == 0.0f) mac |= 1u << bit;
        if (r->u[k] >> 31) mac |= 1u << (bit + 4);
    }
    v->mac = mac;
    uint32_t st = 0;
    if (mac & 0x0f) st |= 1;
    if (mac & 0xf0) st |= 2;
    v->status = (v->status & ~0x3fu) | st | ((v->status | (st << 6)) & 0xfc0);
}

/* MAC flags alone (when no program reads the status flags) */
static inline uint32_t vu_mac(const V4 *r, int dest) {
    uint32_t mac = 0;
    for (int k = 0; k < 4; k++) {
        if (!(dest & (8 >> k))) continue;
        int bit = 3 - k;
        if (r->f[k] == 0.0f) mac |= 1u << bit;
        if (r->u[k] >> 31) mac |= 1u << (bit + 4);
    }
    return mac;
}

static inline uint8_t *vu_dmem(uint32_t qaddr) { return g_vu1_data + ((qaddr & 0x3ff) << 4); }

static inline void vu_store(uint32_t qaddr, const V4 *s, int dest) {
    uint8_t *m = vu_dmem(qaddr);
    for (int k = 0; k < 4; k++) if (dest & (8 >> k)) memcpy(m + 4 * k, &s->u[k], 4);
}
static inline void vu_store_word(uint32_t qaddr, uint32_t w, int dest) {
    uint8_t *m = vu_dmem(qaddr);
    for (int k = 0; k < 4; k++) if (dest & (8 >> k)) memcpy(m + 4 * k, &w, 4);
}
static inline uint32_t vu_load_word(uint32_t qaddr, int dest) {
    const uint8_t *m = vu_dmem(qaddr);
    int k = dest & 8 ? 0 : dest & 4 ? 1 : dest & 2 ? 2 : 3;
    uint32_t w; memcpy(&w, m + 4 * k, 4);
    return w;
}
static inline void vu_write_vf(VU1 *v, int reg, const V4 *x, int dest) {
    if (reg <= 0) return;
    for (int k = 0; k < 4; k++) if (dest & (8 >> k)) v->vf[reg].u[k] = x->u[k];
}
