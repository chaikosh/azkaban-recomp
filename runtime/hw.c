/* EE hardware registers (0x1000xxxx), GS privileged registers (0x1200xxxx). */
#include "rt.h"
int iopb_enabled(void);
uint32_t iopb_sif_get_reg(uint32_t reg);
void iopb_sif_set_reg(uint32_t reg, uint32_t v);
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include "gs.h"

static uint8_t eeregs[0x10000];   /* shadow of 0x10000000..0x1000ffff */
static uint8_t gspriv[0x2000];    /* shadow of 0x12000000..0x12001fff */
static uint32_t gs_csr = 0x551b0000u | 0x8;  /* revision/id bits, FIFO empty */
static uint32_t sif_mscom, sif_smcom, sif_msflg, sif_smflg = 0x60000, sif_ctrl, sif_bd6;
static uint32_t sif_sys[8];
static void sif_poll_reboot(void);

uint32_t kern_dmac_stat(void);
void kern_dmac_write_stat(uint32_t v);
void dma_start(int ch);

static uint64_t wall_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
uint64_t virt_ns(void);
static uint64_t now_ns(void) { return virt_ns(); }   /* timers run on emulated time */

/* ---- timers -------------------------------------------------------------- */
/* count(t) = base + ticks since t0, with compare/overflow events computed
 * analytically.  Events set EQUF/OVFF and raise INTC 9..12. */
typedef struct { uint32_t mode, comp, hold, base; uint64_t t0; uint64_t next_cmp, next_ovf; } Timer;
static Timer tim[4];
static pthread_mutex_t tm = PTHREAD_MUTEX_INITIALIZER;
void kern_timers_changed(void);

static uint32_t timer_rate(uint32_t mode) {
    switch (mode & 3) {
    case 0: return 147456000u;
    case 1: return 147456000u / 16;
    case 2: return 147456000u / 256;
    default: return 15734u;   /* HBLANK, NTSC */
    }
}
static uint64_t ticks_since(const Timer *t, uint64_t now) {
    return (now - t->t0) * timer_rate(t->mode) / 1000000000ull;
}
static uint64_t tick_time(const Timer *t, uint64_t ticks) {
    return t->t0 + (ticks * 1000000000ull + timer_rate(t->mode) - 1) / timer_rate(t->mode);
}
static uint32_t timer_count(int k) {
    Timer *t = &tim[k];
    if (!(t->mode & 0x80)) return t->base & 0xffff;
    uint64_t ticks = ticks_since(t, now_ns());
    if ((t->mode & 0x40) && t->comp) {   /* ZRET: wraps after reaching COMP */
        uint64_t first = ((t->comp - t->base - 1) & 0xffff) + 1;
        if (ticks <= first) return (uint32_t)(t->base + ticks) & 0xffff;
        return (uint32_t)((ticks - first - 1) % (t->comp + 1));
    }
    return (uint32_t)(t->base + ticks) & 0xffff;
}
/* (re)compute the next event tick indices after any register change */
static void timer_rearm(int k) {
    Timer *t = &tim[k];
    t->next_cmp = t->next_ovf = 0;
    if (!(t->mode & 0x80)) return;
    t->next_cmp = ((t->comp - t->base - 1) & 0xffff) + 1;
    t->next_ovf = 0x10000 - (t->base & 0xffff);
}
static void timer_set(int k, uint32_t count) {
    tim[k].base = count & 0xffff;
    tim[k].t0 = now_ns();
    timer_rearm(k);
}

uint64_t hw_timers_next(void) {
    uint64_t best = 0;
    pthread_mutex_lock(&tm);
    for (int k = 0; k < 4; k++) {
        Timer *t = &tim[k];
        if (!(t->mode & 0x80)) continue;
        if ((t->mode & 0x100) && t->next_cmp) { uint64_t e = tick_time(t, t->next_cmp); if (!best || e < best) best = e; }
        if ((t->mode & 0x200) && !(t->mode & 0x40) && t->next_ovf) { uint64_t e = tick_time(t, t->next_ovf); if (!best || e < best) best = e; }
    }
    pthread_mutex_unlock(&tm);
    return best;
}

void hw_timers_fire(void) {
    uint64_t now = now_ns();
    int raise[4] = {0};
    pthread_mutex_lock(&tm);
    for (int k = 0; k < 4; k++) {
        Timer *t = &tim[k];
        if (!(t->mode & 0x80)) continue;
        uint64_t ticks = ticks_since(t, now);
        uint64_t cper = ((t->mode & 0x40) && t->comp) ? (uint64_t)t->comp + 1 : 0x10000;
        /* the interrupt is raised when EQUF/OVFF goes from 0 to 1; while the game
         * leaves a flag set, further compares/overflows stay silent */
        if (t->next_cmp && ticks >= t->next_cmp) {
            if (!(t->mode & 0x400) && (t->mode & 0x100)) raise[k] = 1;
            t->mode |= 0x400;
            while (t->next_cmp <= ticks) t->next_cmp += cper;
        }
        if (t->next_ovf && ticks >= t->next_ovf) {
            if (!(t->mode & 0x40)) {
                if (!(t->mode & 0x800) && (t->mode & 0x200)) raise[k] = 1;
                t->mode |= 0x800;
            }
            while (t->next_ovf <= ticks) t->next_ovf += 0x10000;
        }
    }
    pthread_mutex_unlock(&tm);
    for (int k = 0; k < 4; k++) if (raise[k]) {
        if (g_trace > 2 && k == 0) LOG("[timer] T0 irq at %llu ns (mode %x comp %x base %x next %llu)\n",
                                       (unsigned long long)now, tim[0].mode, tim[0].comp, tim[0].base, (unsigned long long)tim[0].next_cmp);
        kern_raise_intc(9 + k);
    }
}

/* ---- logging of unknown registers ---------------------------------------- */
static void note(const char *what, uint32_t a, uint64_t v) {
    static uint32_t seen[512];
    static int n;
    for (int k = 0; k < n; k++) if (seen[k] == a) return;
    if (n < 512) seen[n++] = a;
    LOG("[hw] first %s %08x = %llx\n", what, a, (unsigned long long)v);
}

static uint32_t sh32(uint8_t *base, uint32_t off) { uint32_t v; memcpy(&v, base + off, 4); return v; }
static void sw32(uint8_t *base, uint32_t off, uint32_t v) { memcpy(base + off, &v, 4); }

void gs_priv_write(uint32_t off, uint64_t v);

/* ---- 32-bit register file -------------------------------------------------- */
uint32_t mmio_r32(uint32_t a) {
    uint32_t p = a & 0x1fffffffu;
    if (p >= 0x10000000u && p < 0x10010000u) {
        uint32_t off = p & 0xffff;
        if (off < 0x2000) {
            int k = off >> 11;
            switch (off & 0x7f0) {
            case 0x00: return timer_count(k);
            case 0x10: return tim[k].mode;
            case 0x20: return tim[k].comp;
            case 0x30: return tim[k].hold;
            }
        }
        if (off >= 0x3c00 && off < 0x3e00) { extern uint32_t vif1_reg_read(uint32_t); return vif1_reg_read(off - 0x3c00); }
        switch (off) {
        case 0xe010: return kern_dmac_stat();
        case 0xf000: return kern_intc_stat();
        case 0xf010: return kern_intc_mask();
        case 0xf200: case 0xf210: case 0xf220: case 0xf230:
            if (iopb_enabled()) return iopb_sif_get_reg(((off >> 4) & 7) + 1);
            switch (off) {
            case 0xf200: return sif_mscom;
            case 0xf210: return sif_smcom;
            case 0xf220: return sif_msflg;
            default: sif_poll_reboot(); return sif_smflg;
            }
        case 0xf240: return sif_ctrl;
        case 0xf260: return sif_bd6;
        case 0xf520: return sh32(eeregs, 0xf590);   /* D_ENABLER mirrors D_ENABLEW */
        case 0x3020: return 0;                       /* GIF_STAT: idle */
        case 0x3c00: return 0;                       /* VIF1_STAT: idle */
        case 0x3800: return 0;                       /* VIF0_STAT */
        case 0x2000: case 0x2004: case 0x2010: case 0x2020: case 0x2030: case 0x2034: { extern uint32_t ipu_read(uint32_t); return ipu_read(off - 0x2000); }
        }
        uint32_t v = sh32(eeregs, off);
        if (g_trace) note("read", p, v);
        return v;
    }
    if (p >= 0x12000000u && p < 0x12002000u) {
        uint32_t off = p & 0x1fff;
        if (off == 0x1000) return gs_csr;
        return sh32(gspriv, off);
    }
    if (p >= 0x1fc00000u && p < 0x20000000u) return 0;   /* BIOS ROM */
    note("read (unmapped page)", p & ~0xffffu, 0);
    static int dumped;
    if (!dumped++) { EE *c = kern_current_ctx(); if (c) rt_dump(c); }
    return 0;
}

void mmio_w32(uint32_t a, uint32_t v) {
    uint32_t p = a & 0x1fffffffu;
    if (p >= 0x10000000u && p < 0x10010000u) {
        uint32_t off = p & 0xffff;
        if (off < 0x2000) {
            int k = off >> 11;
            if (g_trace > 1) LOG("[timer] T%d reg %02x = %08x\n", k, off & 0x7f0, v);
            pthread_mutex_lock(&tm);
            switch (off & 0x7f0) {
            case 0x00: timer_set(k, v); break;
            case 0x10: {
                uint32_t cnt = timer_count(k);
                tim[k].mode = (tim[k].mode & 0xc00 & ~(v & 0xc00)) | (v & 0x3ff);
                timer_set(k, cnt);
                break;
            }
            case 0x20: { uint32_t cnt = timer_count(k); tim[k].comp = v & 0xffff; timer_set(k, cnt); break; }
            case 0x30: tim[k].hold = v & 0xffff; break;
            }
            pthread_mutex_unlock(&tm);
            kern_timers_changed();
            return;
        }
        if (off >= 0x3c00 && off < 0x3e00) { extern void vif1_reg_write(uint32_t, uint32_t); vif1_reg_write(off - 0x3c00, v); }
        if ((off == 0x2000 || off == 0x2010) && g_trace) LOG("[ipu] %s = %08x\n", off == 0x2000 ? "CMD" : "CTRL", v);
        if (off == 0x2000) { extern void ipu_write_cmd(uint32_t); ipu_write_cmd(v); return; }
        if (off == 0x2010) { extern void ipu_write_ctrl(uint32_t); ipu_write_ctrl(v); return; }
        switch (off) {
        case 0xe010: kern_dmac_write_stat(v); return;
        case 0xf000: kern_intc_ack(v); return;
        case 0xf010: kern_intc_toggle_mask(v); return;
        case 0xf200: case 0xf220: case 0xf230:
            if (iopb_enabled()) { iopb_sif_set_reg(((off >> 4) & 7) + 1, v); return; }
            if (off == 0xf200) sif_mscom = v;
            else if (off == 0xf220) sif_msflg |= v;
            else sif_smflg &= ~v;
            return;
        case 0xf240: sif_ctrl = v; return;
        case 0xf260: sif_bd6 = v; return;
        }
        sw32(eeregs, off, v);
        /* DMA channel CHCR with STR set starts a transfer */
        static const uint16_t chcr[10] = {0x8000, 0x9000, 0xa000, 0xb000, 0xb400, 0xc000, 0xc400, 0xc800, 0xd000, 0xd400};
        for (int ch = 0; ch < 10; ch++)
            if (off == chcr[ch]) { if (v & 0x100) dma_start(ch); return; }
        if (g_trace) note("write", p, v);
        return;
    }
    if (p >= 0x12000000u && p < 0x12002000u) {
        uint32_t off = p & 0x1fff;
        if (off == 0x1000) {   /* CSR: write 1 clears SIGNAL/FINISH/HSINT/VSINT/EDWINT, RESET */
            gs_csr &= ~(v & 0x1f);
            return;
        }
        sw32(gspriv, off, v);
        { uint64_t q; memcpy(&q, gspriv + (off & ~7u), 8); gs_priv_write(off & ~7u, q); }   /* debug hooks see 32-bit writes too */
        return;
    }
    note("write (unmapped page)", p & ~0xffffu, v);
}

uint64_t mmio_r64(uint32_t a) {
    uint32_t p = a & 0x1fffffffu;
    if (p >= 0x12000000u && p < 0x12002000u) {
        uint32_t off = p & 0x1fff;
        if (off == 0x1000) return gs_csr;
        uint64_t v; memcpy(&v, gspriv + off, 8); return v;
    }
    return mmio_r32(a) | ((uint64_t)mmio_r32(a + 4) << 32);
}

void mmio_w64(uint32_t a, uint64_t v) {
    uint32_t p = a & 0x1fffffffu;
    if (p >= 0x12000000u && p < 0x12002000u) {
        uint32_t off = p & 0x1fff;
        if (off == 0x1000) { gs_csr &= ~(uint32_t)(v & 0x1f); return; }
        memcpy(gspriv + off, &v, 8);
        gs_priv_write(off, v);
        return;
    }
    mmio_w32(a, (uint32_t)v);
    mmio_w32(a + 4, (uint32_t)(v >> 32));
}

uint8_t mmio_r8(uint32_t a) { return (uint8_t)(mmio_r32(a & ~3u) >> (8 * (a & 3))); }
uint16_t mmio_r16(uint32_t a) { return (uint16_t)(mmio_r32(a & ~3u) >> (8 * (a & 2))); }
void mmio_w8(uint32_t a, uint8_t v) {
    uint32_t w = mmio_r32(a & ~3u), s = 8 * (a & 3);
    mmio_w32(a & ~3u, (w & ~(0xffu << s)) | ((uint32_t)v << s));
}
void mmio_w16(uint32_t a, uint16_t v) {
    uint32_t w = mmio_r32(a & ~3u), s = 8 * (a & 2);
    mmio_w32(a & ~3u, (w & ~(0xffffu << s)) | ((uint32_t)v << s));
}

void fifo_write(int which, U128 v);   /* 0 = VIF0, 1 = VIF1, 2 = GIF, 3 = IPU in */

U128 mmio_r128(uint32_t a) {
    U128 v;
    uint32_t p = a & 0x1fffffffu;
    memset(&v, 0, sizeof v);
    if (p >= 0x10007000u && p < 0x10007010u) { extern uint32_t ipu_out_qwords(uint8_t *, uint32_t); ipu_out_qwords((uint8_t *)&v, 1); return v; }
    if (p >= 0x10005000u && p < 0x10006000u) {   /* VIF1 FIFO read: GS download, after the GS work issued before it */
        extern void gs_read_image(uint8_t *, uint32_t); extern void render_wait_idle(void); extern pthread_mutex_t g_gs_lock;
        render_wait_idle();
        pthread_mutex_lock(&g_gs_lock); gs_read_image((uint8_t *)&v, 1); pthread_mutex_unlock(&g_gs_lock);
        return v;
    }
    if (p >= 0x10004000u && p < 0x10008000u) { note("fifo read", p, 0); return v; }
    for (int k = 0; k < 4; k++) v.w[k] = mmio_r32(a + 4 * k);
    return v;
}

void mmio_w128(uint32_t a, U128 v) {
    uint32_t p = a & 0x1fffffffu;
    switch (p & ~0xfffu) {
    case 0x10004000u: fifo_write(0, v); return;
    case 0x10005000u: fifo_write(1, v); return;
    case 0x10006000u: fifo_write(2, v); return;
    case 0x10007000u: if (p & 0x10) { fifo_write(3, v); return; } break;
    }
    for (int k = 0; k < 4; k++) mmio_w32(a + 4 * k, v.w[k]);
}

/* ---- SIF registers through syscalls --------------------------------------- */
static uint64_t iop_reboot_at;   /* IOP "finishes" rebooting a little after the reset command */
void hw_sif_iop_reset(void) { iop_reboot_at = wall_ns() + 5000000ull; }
static void sif_poll_reboot(void) {
    if (iop_reboot_at && wall_ns() >= iop_reboot_at) { sif_smflg |= 0x70000; iop_reboot_at = 0; }
}

int64_t hw_sif_get_reg(uint32_t reg) {
    sif_poll_reboot();
    switch (reg) {
    case 1: return sif_mscom;
    case 2: return sif_smcom;
    case 3: return sif_msflg;
    case 4: return sif_smflg;
    case 5: return sif_ctrl;
    case 7: return sif_bd6;
    }
    if (reg >= 0x80000000u && reg < 0x80000008u) return sif_sys[reg & 7];
    LOG("[sif] get unknown reg %08x\n", reg);
    return 0;
}

void hw_sif_set_reg(uint32_t reg, uint32_t v) {
    switch (reg) {
    case 1: sif_mscom = v; return;
    case 2: sif_smcom = v; return;
    case 3: sif_msflg |= v; return;
    case 4: sif_smflg &= ~v; return;
    case 5: sif_ctrl = v; return;
    case 7: sif_bd6 = v; return;
    }
    if (reg >= 0x80000000u && reg < 0x80000008u) { sif_sys[reg & 7] = v; return; }
    LOG("[sif] set unknown reg %08x = %08x\n", reg, v);
}

void hw_sif_set_smcom(uint32_t v) { sif_smcom = v; }
void hw_sif_set_smflag(uint32_t bits) { sif_smflg |= bits; }

/* ---- display -------------------------------------------------------------- */
static int crt_interlace, crt_mode, crt_field;
void hw_set_crt(int interlace, int mode, int field) {
    crt_interlace = interlace; crt_mode = mode; crt_field = field;
    LOG("[gs] SetGsCrt interlace=%d mode=%d field=%d\n", interlace, mode, field);
}

void pad_vblank(void);
void iop_tick(void);
int iopb_enabled(void);
void iopb_vblank(int start);
void iopb_init(const char *iso, const char *memcard);
uint32_t iopb_sif_get_reg(uint32_t reg);
void iopb_sif_set_reg(uint32_t reg, uint32_t v);

void display_vsync(void);

void hw_vblank(int start) {
    { extern volatile int g_usr1_req; extern void rt_usr1_work(void); if (g_usr1_req) { g_usr1_req = 0; rt_usr1_work(); } }
    if (start) {
        {   /* HP_DUMPAT=vsync[,vsync...]: run the SIGUSR1 diagnostic dump (HP_VRAMDUMP/HP_RAMDUMP/HP_PEEK) at those vsyncs */
            extern unsigned long long g_vsyncs; extern void rt_usr1_work(void);
            static const char *da = (const char *)1;
            if (da == (const char *)1) da = getenv("HP_DUMPAT");
            for (const char *p = da; p && *p; ) {
                if (strtoull(p, NULL, 10) == g_vsyncs + 1) { rt_usr1_work(); break; }
                p = strchr(p, ','); if (p) p++;
            }
        }
        display_vsync();
        {   /* HP_EXITAT=vsync: end the run there (bounded debug runs) */
            extern unsigned long long g_vsyncs;
            static long long ea = -2; if (ea == -2) ea = getenv("HP_EXITAT") ? atoll(getenv("HP_EXITAT")) : -1;
            if (ea >= 0 && (long long)g_vsyncs >= ea) { LOG("[exit] HP_EXITAT reached at vsync %llu\n", g_vsyncs); fflush(NULL); _exit(0); }
        }
        if (iopb_enabled()) iopb_vblank(1);
        else { pad_vblank(); iop_tick(); }
        static int frames;
        extern void dma_stats(void);
        if (getenv("HP_STATS") && ++frames % 120 == 0) dma_stats();
        gs_csr ^= 0x2000;   /* FIELD */
        gs_csr |= 0x8;      /* VSINT */
    } else if (iopb_enabled()) iopb_vblank(0);
}

/* GS SIGNAL/FINISH events -> CSR bits and the GS interrupt (INTC 0) */
uint64_t kern_gs_imr(void);
void gs_signal(uint64_t v) {
    uint32_t id = (uint32_t)v, mask = (uint32_t)(v >> 32);
    uint32_t cur = (uint32_t)hw_gs_priv(0x1080);
    cur = (cur & ~mask) | (id & mask);
    memcpy(gspriv + 0x1080, &cur, 4);
    gs_csr |= 1;
    if (!(kern_gs_imr() & 0x100)) kern_raise_intc(0);
}
void gs_finish(void) {
    gs_csr |= 2;
    if (!(kern_gs_imr() & 0x200)) kern_raise_intc(0);
}
uint64_t hw_gs_priv(uint32_t off) { uint64_t v; memcpy(&v, gspriv + (off & 0x1ff8), 8); return v; }

void hw_init(void) {
    sw32(eeregs, 0xf590, 0x1201);
    extern void ipu_init(void);
    ipu_init();
    if (iopb_enabled()) {
        const char *iso = getenv("HP_ISO") ? getenv("HP_ISO") : "iso/Harry Potter and the Prisoner of Azkaban (USA).iso";
        const char *mc = getenv("HP_MEMCARD") ? getenv("HP_MEMCARD") : "build/memcard0.ps2";
        iopb_init(iso, mc);
    } else iop_init();
}

/* Register write without side effects (used by the DMA engine). */
void mmio_w32_raw(uint32_t a, uint32_t v) { sw32(eeregs, a & 0xffff, v); }
uint32_t hw_sif_sysreg(int k) { return sif_sys[k & 7]; }
