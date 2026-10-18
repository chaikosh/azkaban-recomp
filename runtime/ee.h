/* Emotion Engine context and the helpers generated code is built on. */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <float.h>

#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

typedef union {
    uint64_t d[2]; int64_t sd[2];
    uint32_t w[4]; int32_t sw[4];
    uint16_t h[8]; int16_t sh[8];
    uint8_t b[16]; int8_t sbt[16];
    unsigned __int128 q;
} U128;

typedef union { float f; uint32_t u; int32_t i; } FPR;
typedef union { float f[4]; uint32_t u[4]; int32_t i[4]; U128 q; } VF;

typedef struct EE {
    U128 r[32];
    U128 hi, lo;          /* d[0] = HI/LO, d[1] = HI1/LO1 */
    uint64_t sa;          /* funnel shift amount in bytes */
    FPR f[32];
    FPR acc;
    uint32_t fcr31;       /* bit 23 = condition */
    /* VU0 macro mode */
    VF vf[32];
    VF vacc;
    uint16_t vi[16];
    float vq, vp, vi_i;   /* Q, P, I */
    uint32_t vstatus, vmac, vclip, vr;
    /* bookkeeping */
    uint32_t pc;          /* last syscall/trap site, for diagnostics */
    uint32_t pcring[512]; /* TRACE_PC: recent basic-block starts */
    uint32_t pcidx;
    uint32_t thread;      /* kernel thread id running on this context */
    void *host;           /* runtime-private */
} EE;

typedef void (*GuestFn)(EE *);

/* ---- guest memory: 64 KiB pages, NULL = memory-mapped I/O --------------- */
extern uint8_t *g_pt[65536];
extern uint8_t *g_ram;    /* 32 MiB main RAM */
extern volatile int g_irq_pending;

uint8_t mmio_r8(uint32_t a);  uint16_t mmio_r16(uint32_t a);
uint32_t mmio_r32(uint32_t a); uint64_t mmio_r64(uint32_t a); U128 mmio_r128(uint32_t a);
void mmio_w8(uint32_t a, uint8_t v);  void mmio_w16(uint32_t a, uint16_t v);
void mmio_w32(uint32_t a, uint32_t v); void mmio_w64(uint32_t a, uint64_t v);
void mmio_w128(uint32_t a, U128 v);

static inline uint8_t *HP(uint32_t a) { uint8_t *b = g_pt[a >> 16]; return b ? b + (a & 0xFFFF) : 0; }

#ifdef WATCH
extern uint32_t g_watch;
void rt_watch_hit(uint32_t a, uint64_t v, int size);
#define WATCHED(a, v, sz) do { if (UNLIKELY(((a) & 0x1ffffff0u) == g_watch)) rt_watch_hit(a, (uint64_t)(v), sz); } while (0)
#else
#define WATCHED(a, v, sz) ((void)0)
#endif

#define MEMFN(T, n, sz)                                                                 \
    static inline T ld##n(uint32_t a) { uint8_t *p = HP(a); T v;                         \
        if (LIKELY(p)) { memcpy(&v, p, sz); return v; } return mmio_r##n(a); }           \
    static inline void st##n(uint32_t a, T v) { uint8_t *p = HP(a); WATCHED(a, v, sz);  \
        if (LIKELY(p)) { memcpy(p, &v, sz); return; } mmio_w##n(a, v); }
MEMFN(uint8_t, 8, 1)
MEMFN(uint16_t, 16, 2)
MEMFN(uint32_t, 32, 4)
MEMFN(uint64_t, 64, 8)
#undef MEMFN
static inline U128 ld128(uint32_t a) { a &= ~15u; uint8_t *p = HP(a); U128 v;
    if (LIKELY(p)) { memcpy(&v, p, 16); return v; } return mmio_r128(a); }
static inline void st128(uint32_t a, U128 v) { a &= ~15u; uint8_t *p = HP(a); WATCHED(a, v.d[0], 16);
    if (LIKELY(p)) { memcpy(p, &v, 16); return; } mmio_w128(a, v); }

/* ---- register helpers --------------------------------------------------- */
#define R(n)   (c->r[n].d[0])
#define RS(n)  ((int64_t)c->r[n].d[0])
#define RW(n)  ((uint32_t)c->r[n].d[0])
#define RSW(n) ((int32_t)c->r[n].d[0])
#define SX(x)  ((uint64_t)(int64_t)(int32_t)(uint32_t)(x))
#define EA(b, o) ((uint32_t)(RW(b) + (uint32_t)(int32_t)(o)))

/* Unaligned load/store halves (little endian). */
static inline void op_lwl(EE *c, int rt, uint32_t a) {
    static const uint32_t M[4] = {0xffffff, 0xffff, 0xff, 0}; static const uint8_t S[4] = {24, 16, 8, 0};
    uint32_t s = a & 3, m = ld32(a & ~3u);
    if (rt) R(rt) = SX((RW(rt) & M[s]) | (m << S[s]));
}
static inline void op_lwr(EE *c, int rt, uint32_t a) {
    static const uint32_t M[4] = {0, 0xff000000, 0xffff0000, 0xffffff00}; static const uint8_t S[4] = {0, 8, 16, 24};
    uint32_t s = a & 3, m = ld32(a & ~3u);
    if (!rt) return;
    m = (RW(rt) & M[s]) | (m >> S[s]);
    if (s == 0) R(rt) = SX(m); else c->r[rt].w[0] = m;
}
static inline void op_ldl(EE *c, int rt, uint32_t a) {
    uint32_t s = a & 7; uint64_t m = ld64(a & ~7u);
    uint64_t mask = s == 7 ? 0 : (0x00ffffffffffffffull >> (8 * s));
    if (rt) R(rt) = (R(rt) & mask) | (m << (56 - 8 * s));
}
static inline void op_ldr(EE *c, int rt, uint32_t a) {
    uint32_t s = a & 7; uint64_t m = ld64(a & ~7u);
    uint64_t mask = s == 0 ? 0 : (~0ull << (64 - 8 * s));
    if (rt) R(rt) = (R(rt) & mask) | (m >> (8 * s));
}
static inline void op_swl(EE *c, int rt, uint32_t a) {
    static const uint32_t M[4] = {0xffffff00, 0xffff0000, 0xff000000, 0}; static const uint8_t S[4] = {24, 16, 8, 0};
    uint32_t s = a & 3, m = ld32(a & ~3u);
    st32(a & ~3u, (RW(rt) >> S[s]) | (m & M[s]));
}
static inline void op_swr(EE *c, int rt, uint32_t a) {
    static const uint32_t M[4] = {0, 0xff, 0xffff, 0xffffff}; static const uint8_t S[4] = {0, 8, 16, 24};
    uint32_t s = a & 3, m = ld32(a & ~3u);
    st32(a & ~3u, (RW(rt) << S[s]) | (m & M[s]));
}
static inline void op_sdl(EE *c, int rt, uint32_t a) {
    uint32_t s = a & 7; uint64_t m = ld64(a & ~7u);
    uint64_t mask = s == 7 ? 0 : (~0ull << (8 * (s + 1)));
    st64(a & ~7u, (R(rt) >> (56 - 8 * s)) | (m & mask));
}
static inline void op_sdr(EE *c, int rt, uint32_t a) {
    uint32_t s = a & 7; uint64_t m = ld64(a & ~7u);
    uint64_t mask = s == 0 ? 0 : (~0ull >> (64 - 8 * s));
    st64(a & ~7u, (R(rt) << (8 * s)) | (m & mask));
}

/* ---- integer multiply/divide ------------------------------------------- */
static inline void op_mult(EE *c, int rd, int32_t a, int32_t b, int pipe) {
    int64_t p = (int64_t)a * b;
    c->lo.d[pipe] = SX(p); c->hi.d[pipe] = SX(p >> 32);
    if (rd) R(rd) = c->lo.d[pipe];
}
static inline void op_multu(EE *c, int rd, uint32_t a, uint32_t b, int pipe) {
    uint64_t p = (uint64_t)a * b;
    c->lo.d[pipe] = SX(p); c->hi.d[pipe] = SX(p >> 32);
    if (rd) R(rd) = c->lo.d[pipe];
}
static inline void op_madd(EE *c, int rd, int32_t a, int32_t b, int pipe) {
    int64_t acc = (int64_t)(((uint64_t)(uint32_t)c->hi.d[pipe] << 32) | (uint32_t)c->lo.d[pipe]);
    int64_t p = acc + (int64_t)a * b;
    c->lo.d[pipe] = SX(p); c->hi.d[pipe] = SX(p >> 32);
    if (rd) R(rd) = c->lo.d[pipe];
}
static inline void op_maddu(EE *c, int rd, uint32_t a, uint32_t b, int pipe) {
    uint64_t acc = ((uint64_t)(uint32_t)c->hi.d[pipe] << 32) | (uint32_t)c->lo.d[pipe];
    uint64_t p = acc + (uint64_t)a * b;
    c->lo.d[pipe] = SX(p); c->hi.d[pipe] = SX(p >> 32);
    if (rd) R(rd) = c->lo.d[pipe];
}
static inline void op_div(EE *c, int32_t a, int32_t b, int pipe) {
    if (b == 0) { c->lo.d[pipe] = SX(a < 0 ? 1 : -1); c->hi.d[pipe] = SX(a); }
    else if (a == INT32_MIN && b == -1) { c->lo.d[pipe] = SX(INT32_MIN); c->hi.d[pipe] = 0; }
    else { c->lo.d[pipe] = SX(a / b); c->hi.d[pipe] = SX(a % b); }
}
static inline void op_divu(EE *c, uint32_t a, uint32_t b, int pipe) {
    if (b == 0) { c->lo.d[pipe] = SX(0xffffffffu); c->hi.d[pipe] = SX(a); }
    else { c->lo.d[pipe] = SX(a / b); c->hi.d[pipe] = SX(a % b); }
}

/* ---- FPU (no Inf/NaN on the EE: results clamp to +-FLT_MAX) ----------- */
static inline float fclamp(float x) {
    if (UNLIKELY(!__builtin_isfinite(x))) {
        FPR r; r.f = x; r.u = (r.u & 0x80000000u) | 0x7f7fffffu; return r.f;
    }
    return x;
}
static inline float fpu_div(EE *c, float a, float b) {
    if (UNLIKELY(b == 0.0f)) { FPR r, x, y; x.f = a; y.f = b; r.u = ((x.u ^ y.u) & 0x80000000u) | 0x7f7fffffu;
        c->fcr31 |= 0x30000; return r.f; }
    return fclamp(a / b);
}
static inline float fpu_sqrt(EE *c, float b) { return sqrtf(fabsf(b)); }
static inline float fpu_rsqrt(EE *c, float a, float b) {
    float s = sqrtf(fabsf(b));
    if (UNLIKELY(s == 0.0f)) { FPR r, x; x.f = a; r.u = (x.u & 0x80000000u) | 0x7f7fffffu; return r.f; }
    return fclamp(a / s);
}
static inline int32_t f2i_sat(float x) {
    if (x >= 2147483648.0f) return INT32_MAX;
    if (x <= -2147483648.0f) return INT32_MIN;
    if (x != x) return INT32_MAX;
    return (int32_t)x;
}
#define FCOND(c) (((c)->fcr31 >> 23) & 1)
#define SETFC(c, v) ((c)->fcr31 = ((c)->fcr31 & ~0x800000u) | ((v) ? 0x800000u : 0))

/* ---- MMI ---------------------------------------------------------------- */
static inline uint32_t plzcw1(uint32_t v) {
    /* count leading bits equal to the sign bit, minus one */
    uint32_t x = (v & 0x80000000u) ? ~v : v;
    return x ? (uint32_t)__builtin_clz(x) - 1 : 31;
}
static inline U128 op_qfsrv(U128 rs, U128 rt, uint32_t sa) {
    uint32_t s = (sa & 15) * 8; U128 r;
    if (!s) return rt;
    r.q = (rt.q >> s) | (rs.q << (128 - s));
    return r;
}
static inline int16_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }

/* ---- VU0 macro mode ----------------------------------------------------- */
static inline float vclamp(float x) { return fclamp(x); }
static inline float vu_toi_f(int32_t i, int sh) { return (float)i / (float)(1 << sh); }
static inline int32_t vu_ftoi(float f, int sh) { return f2i_sat(f * (float)(1 << sh)); }
static inline float vu_div(EE *c, float a, float b) {
    if (UNLIKELY(b == 0.0f)) { FPR r, x, y; x.f = a; y.f = b; r.u = ((x.u ^ y.u) & 0x80000000u) | 0x7f7fffffu; return r.f; }
    return fclamp(a / b);
}
uint32_t vu0_cfc2(EE *c, int reg);
void vu0_ctc2(EE *c, int reg, uint32_t v);
extern uint8_t *g_vu0_mem;    /* VU0 data memory, inside the 0x11000000 page */
static inline void vu0_clip(EE *c, const VF *fs, float w) {
    w = fabsf(w);
    uint32_t f = 0;
    if (fs->f[0] > +w) f |= 0x01; if (fs->f[0] < -w) f |= 0x02;
    if (fs->f[1] > +w) f |= 0x04; if (fs->f[1] < -w) f |= 0x08;
    if (fs->f[2] > +w) f |= 0x10; if (fs->f[2] < -w) f |= 0x20;
    c->vclip = ((c->vclip << 6) | f) & 0xffffff;
}

/* ---- runtime entry points used by generated code ------------------------ */
void rt_call(EE *c, uint32_t addr);                 /* indirect call */
void rt_syscall(EE *c, uint32_t pc);
void rt_break(EE *c, uint32_t pc, uint32_t code);
void rt_trap(EE *c, uint32_t pc, const char *what);
void rt_bad_jump(EE *c, uint32_t pc, uint32_t target) __attribute__((noreturn));
void rt_ra_mismatch(EE *c, uint32_t pc, uint32_t expect);
void rt_irq_poll(EE *c);
uint32_t rt_mfc0(EE *c, int reg, int sel);
void rt_mtc0(EE *c, int reg, uint32_t v);
int rt_cpcond0(EE *c);
void rt_ei(EE *c); void rt_di(EE *c); void rt_eret(EE *c, uint32_t pc);
void rt_unimpl(EE *c, uint32_t pc, const char *op) __attribute__((noreturn));

#define POLL() do { if (UNLIKELY(g_irq_pending)) rt_irq_poll(c); } while (0)
