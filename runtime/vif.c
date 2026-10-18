/* VIF1: command stream from DMA channel 1 / the VIF1 FIFO into VU1 and PATH2. */
#include "rt.h"
#include "gs.h"
#include <string.h>
#include <stdlib.h>

extern uint8_t *g_vu1_code, *g_vu1_data;
void vu1_run(uint32_t start, uint32_t top, uint32_t itop);
void kern_raise_intc(int line);

typedef struct {
    uint32_t cl, wl, mode, mask, row[4], col[4];
    uint32_t base, ofst, tops, top, itops, itop, dbf, mark, last_pc;
    /* command in progress */
    uint32_t cmd, num, imm, need, got;   /* `need` data words still expected */
    uint32_t code, mark_seen;
    uint32_t data[4];
    /* UNPACK state */
    uint32_t up_addr, up_cnt, up_cyc, up_vn, up_vl, up_m, up_usn, up_bits, up_total;
    /* DIRECT */
    U128 dq; int dq_n;
    uint32_t mpg_addr;
} Vif;

static Vif v1;
unsigned long long g_vif1_words;
#include <pthread.h>
/* Words arriving while the VIF is stalled (or behind stalled data) wait here. */
#define VQ_SIZE (1u << 20)
static uint32_t *vq;
static uint32_t vq_head, vq_tail;
static int dma1_deferred;
static uint32_t dma1_qwords;
void kern_after(uint64_t ns, void (*fn)(int), int arg);
void dma_finish(int ch);

static int stalled, stall_next;
/* FLUSHA waits until PATH3 is idle.  GIF DMA transfers run on the render thread, so one the game
   has started may not have reached the GS yet when the VIF gets to the FLUSHA: the VIF then stops
   (VGW) and the render thread resumes it once the transfer is through (vif1_path3_idle).  The movie
   player depends on this: it kicks the upload of the last texture strips and draws them right after
   a FLUSHA, with no VIF interrupt in between. */
static int path3_wait;
uint32_t vif1_reg_read(uint32_t off) {
    switch (off) {   /* register map per the EE manual: STAT 00 FBRST 10 ERR 20 MARK 30 CYCLE 40 MODE 50 NUM 60 MASK 70 CODE 80 ... */
    case 0x00: return (stalled ? 0xc00 : 0) | (path3_wait ? 0x8 : 0) | (v1.mark_seen ? 0x40 : 0);   /* VIS+INT while stalled by an i bit, VGW, MRK */
    case 0x30: return v1.mark;
    case 0x40: return v1.cl | (v1.wl << 8);
    case 0x50: return v1.mode;
    case 0x60: return v1.num;
    case 0x70: return v1.mask;
    case 0x80: return v1.code;
    case 0x90: return v1.itops;
    case 0xa0: return v1.base;
    case 0xb0: return v1.ofst;
    case 0xc0: return v1.tops;
    case 0xd0: return v1.itop;
    case 0xe0: return v1.top;
    }
    if (off >= 0x100 && off < 0x140) return v1.row[(off - 0x100) >> 4];
    if (off >= 0x140 && off < 0x180) return v1.col[(off - 0x140) >> 4];
    return 0;
}

static void vif1_drain_locked(void);
extern pthread_mutex_t g_gs_lock;
#define RENDER_JOB_VIF1_DRAIN 16
/* STC runs the queued VIF1 data right away on the EE thread: the game waits for it anyway, and
   that is faster than handing it to the render thread.  HP_VIFASYNC=1 resumes it there instead. */
static int sync_drain(void) { static int s = -1; if (s < 0) s = getenv("HP_VIFASYNC") == NULL; return s; }
void vif1_drain_job(void) {   /* render thread */
    pthread_mutex_lock(&g_gs_lock);
    vif1_drain_locked();
    pthread_mutex_unlock(&g_gs_lock);
}

void vif1_reg_write(uint32_t off, uint32_t val) {
    if (off == 0x30) { v1.mark = val; v1.mark_seen = 0; }   /* writing MARK clears MRK */
    else if (off == 0x10) {
        pthread_mutex_lock(&g_gs_lock);
        if (val & 1) { v1.need = 0; v1.cmd = 0; stalled = stall_next = path3_wait = 0; vq_head = vq_tail = 0; }   /* RST */
        int resume = 0;
        if (val & 8) {   /* STC: the VIF goes on with the queued data */
            { extern void synclog(const char *, ...); synclog("vif1 STC (queued %u words)", vq_tail - vq_head); }
            stalled = 0;
            resume = vq_head != vq_tail || dma1_deferred;
            /* only when no earlier DMA (e.g. the PATH3 texture upload the interrupt handler has just
               kicked) is still waiting for the render thread; otherwise resume behind it */
            extern int render_idle(void);
            if (sync_drain() && resume && render_idle()) { vif1_drain_locked(); resume = 0; }
        }
        pthread_mutex_unlock(&g_gs_lock);
        if (resume) { extern void render_post(int job); render_post(RENDER_JOB_VIF1_DRAIN); }
    }
}

static inline uint32_t sext(uint32_t v, int bits, int usn) {
    if (usn) return v;
    return (uint32_t)((int32_t)(v << (32 - bits)) >> (32 - bits));
}

/* Write one unpacked vector (n components in c[]) to VU memory at element index. */
static void unpack_store(uint32_t qaddr, const uint32_t *c, int ncomp, uint32_t cycle_row, int filler) {
    uint32_t *dst = (uint32_t *)(g_vu1_data + ((qaddr & 0x3ff) << 4));
    for (int k = 0; k < 4; k++) {
        uint32_t m = v1.up_m ? (v1.mask >> (2 * (k + 4 * (cycle_row > 3 ? 3 : cycle_row)))) & 3 : 0;
        uint32_t val;
        if (filler) {
            /* filling write: data components are absent; masked as row/col or keep */
            if (m == 0) m = 1;
        }
        switch (m) {
        case 0: {
            uint32_t d = k < ncomp ? c[k] : (ncomp == 1 ? c[0] : (ncomp == 2 ? c[k & 1] : dst[k]));
            if (ncomp == 3 && k == 3) d = 0;   /* V3 writes W = 0 (as ps2-web-recomp does) */
            if (v1.mode == 1) d += v1.row[k];
            else if (v1.mode == 2) { d += v1.row[k]; v1.row[k] = d; }
            val = d;
            break;
        }
        case 1: val = v1.row[k]; break;
        case 2: val = v1.col[cycle_row > 3 ? 3 : cycle_row]; break;
        default: continue;
        }
        dst[k] = val;
    }
}

static int elem_bits(void) {
    int comps = (int)v1.up_vn + 1;
    if (v1.up_vl == 3) return 16;   /* V4-5 */
    return comps * (32 >> v1.up_vl);
}

static uint8_t ub[32];
static int ubn;

/* HP_VIFSTATS: which UNPACK formats take the per-element path (vn, vl, mask, mode, wl > cl) */
static unsigned long long slow_fmt[4][4][2][4][2];
void vif_slow_report(void) {
    if (!getenv("HP_VIFSTATS")) return;
    for (int a = 0; a < 4; a++) for (int b = 0; b < 4; b++) for (int m = 0; m < 2; m++) for (int md = 0; md < 4; md++) for (int f = 0; f < 2; f++)
        if (slow_fmt[a][b][m][md][f]) {
            LOG("[vif1] slow unpack V%d-%d mask %d mode %d %s: %llu elements\n", a + 1, b == 3 ? 5 : 32 >> b, m, md, f ? "fill" : "skip", slow_fmt[a][b][m][md][f]);
            slow_fmt[a][b][m][md][f] = 0;
        }
}
static void unpack_element(const uint8_t *e) {
    uint32_t c[4];
    slow_fmt[v1.up_vn & 3][v1.up_vl & 3][(v1.up_m && v1.mask) ? 1 : 0][v1.mode & 3][(v1.wl ? v1.wl : 1) > (v1.cl ? v1.cl : 1)]++;
    int ncomp = (int)v1.up_vn + 1;
    if (v1.up_vl == 3) {
        uint32_t v = e[0] | (e[1] << 8);
        c[0] = (v & 0x1f) << 3; c[1] = ((v >> 5) & 0x1f) << 3; c[2] = ((v >> 10) & 0x1f) << 3; c[3] = (v >> 15) << 7;
        ncomp = 4;
    } else {
        int b = 32 >> v1.up_vl, nb = b / 8;
        for (int k = 0; k < ncomp; k++) {
            uint32_t x = 0;
            for (int j = 0; j < nb; j++) x |= (uint32_t)e[k * nb + j] << (8 * j);
            c[k] = b == 32 ? x : sext(x, b, (int)v1.up_usn);
        }
    }
    uint32_t cl = v1.cl ? v1.cl : 1, wl = v1.wl ? v1.wl : 1;
    uint32_t idx = v1.up_cnt;
    if (wl <= cl) {
        unpack_store(v1.up_addr + (idx / wl) * cl + (idx % wl), c, ncomp, idx % wl, 0);
    } else {
        /* filling write: CL data vectors, then (WL - CL) filler vectors per cycle */
        uint32_t cyc = idx / cl, pos = idx % cl;
        unpack_store(v1.up_addr + cyc * wl + pos, c, ncomp, pos, 0);
        if (pos == cl - 1)
            for (uint32_t f = cl; f < wl; f++) unpack_store(v1.up_addr + cyc * wl + f, c, ncomp, f, 1);
    }
    v1.up_cnt++;
}

/* Bulk UNPACK: reads the elements straight from the DMA words as a byte stream (an element may
   straddle words; a partial one is carried in ub/ubn exactly like the per-word path), for every
   format but V4-5 and filling writes (WL > CL).  Plain writes (no mask, MODE 0) are stored
   directly; masked or offset writes go through unpack_store.  Returns the data words consumed
   (0: use the per-word path).  HP_VIFSLOW=1 turns it off. */
static int fast_off = -1;
static inline void fast_store(const uint8_t *e, uint32_t ncomp, uint32_t bits, uint32_t idx, uint32_t cl, uint32_t wl, int plain) {
    uint32_t c[4];
    if (bits == 32) memcpy(c, e, ncomp * 4);
    else if (bits == 16)
        for (uint32_t k = 0; k < ncomp; k++) { uint32_t x = e[2 * k] | ((uint32_t)e[2 * k + 1] << 8); c[k] = v1.up_usn ? x : (uint32_t)(int32_t)(int16_t)x; }
    else
        for (uint32_t k = 0; k < ncomp; k++) c[k] = v1.up_usn ? e[k] : (uint32_t)(int32_t)(int8_t)e[k];
    uint32_t row = wl == cl ? 0 : idx % wl;
    uint32_t qaddr = v1.up_addr + (wl == cl ? idx : (idx / wl) * cl + row);
    if (!plain) { unpack_store(qaddr, c, (int)ncomp, wl == cl ? idx % wl : row, 0); return; }
    uint32_t *dst = (uint32_t *)(g_vu1_data + ((qaddr & 0x3ff) << 4));
    switch (ncomp) {
    case 4: dst[0] = c[0]; dst[1] = c[1]; dst[2] = c[2]; dst[3] = c[3]; break;
    case 3: dst[0] = c[0]; dst[1] = c[1]; dst[2] = c[2]; dst[3] = 0; break;
    case 2: dst[0] = c[0]; dst[1] = c[1]; dst[2] = c[0]; dst[3] = c[1]; break;
    default: dst[0] = dst[1] = dst[2] = dst[3] = c[0]; break;
    }
}
static uint32_t unpack_fast(const uint32_t *w, uint32_t nw) {
    if (fast_off < 0) fast_off = getenv("HP_VIFSLOW") != NULL;
    if (fast_off || v1.up_vl == 3) return 0;
    uint32_t cl = v1.cl ? v1.cl : 1, wl = v1.wl ? v1.wl : 1;
    if (wl > cl) return 0;
    uint32_t left = v1.up_total - v1.up_cnt;
    if (!left) return 0;
    uint32_t ncomp = v1.up_vn + 1, bits = 32u >> v1.up_vl, ebytes = ncomp * bits / 8;
    int plain = !(v1.up_m && v1.mask) && !v1.mode;
    const uint8_t *src = (const uint8_t *)w;
    size_t avail = (size_t)nw * 4, pos = 0;
    if (ubn) {   /* finish the element the per-word path started */
        size_t need = ebytes - (size_t)ubn;
        if (need > avail) return 0;
        uint8_t tmp[16];
        memcpy(tmp, ub, (size_t)ubn);
        memcpy(tmp + ubn, src, need);
        fast_store(tmp, ncomp, bits, v1.up_cnt, cl, wl, plain);
        v1.up_cnt++; left--;
        pos = need;
        ubn = 0;
    }
    while (left && pos + ebytes <= avail) {
        fast_store(src + pos, ncomp, bits, v1.up_cnt, cl, wl, plain);
        v1.up_cnt++; left--;
        pos += ebytes;
    }
    uint32_t words = (uint32_t)((pos + 3) / 4);
    if (!words) return 0;
    size_t extra = (size_t)words * 4 - pos;   /* rest of the last word: the next element's start, or padding */
    if (extra) { memcpy(ub, src + pos, extra); ubn = (int)extra; }
    return words;
}

static void vif1_mscal(uint32_t addr_pairs) {
    v1.top = v1.tops;
    v1.itop = v1.itops;
    /* double buffering */
    v1.tops = v1.dbf ? v1.base : v1.base + v1.ofst;
    v1.dbf ^= 1;
    v1.last_pc = addr_pairs;
    extern double g_prof[8], g_vu1_kick_time;
    struct timespec a, b; clock_gettime(CLOCK_UPTIME_RAW, &a);
    double k0 = g_vu1_kick_time;
    vu1_run(addr_pairs, v1.top, v1.itop);
    clock_gettime(CLOCK_UPTIME_RAW, &b);
    g_prof[4] += (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) * 1e-9 - (g_vu1_kick_time - k0);   /* VU1 alone, without GS drawing */
}

/* Begin the command in `w`; returns number of data words it needs. */
unsigned g_vifcmd_count[128];

void vif_stats(void) {
    static unsigned last[128];
    LOG("[stats] vif1 state cmd %02x need %u got %u up %u/%u | cmds:", v1.cmd, v1.need, v1.got, v1.up_cnt, v1.up_total);
    for (int k = 0; k < 128; k++) if (g_vifcmd_count[k] != last[k]) { LOG(" %02x=%u", k, g_vifcmd_count[k] - last[k]); last[k] = g_vifcmd_count[k]; }
    LOG("\n");
}

static void vif1_command(uint32_t w) {
    uint32_t cmd = (w >> 24) & 0x7f, num = (w >> 16) & 0xff, imm = w & 0xffff;
    g_vifcmd_count[cmd]++;
    v1.code = w;
    {   /* HP_VIFCMDS=vsync[,n]: log every VIF1 command from that vsync */
        static long from = -2, left = 0;
        if (from == -2) { const char *e = getenv("HP_VIFCMDS"); from = e ? atol(e) : -1; left = (e && strchr(e, ',')) ? atol(strchr(e, ',') + 1) : 400; }
        extern unsigned long long g_vsyncs;
        if (from >= 0 && (long)g_vsyncs >= from && left > 0) { left--;
            LOG("[vifcmd v%llu] %08x cmd %02x num %u imm %04x | cl %u wl %u base %03x ofst %03x tops %03x itops %03x dbf %u\n", g_vsyncs, w, cmd, num, imm,
                v1.cl, v1.wl, v1.base, v1.ofst, v1.tops, v1.itops, v1.dbf); }
    }
    if (w & 0x80000000u) stall_next = 1;       /* i bit: interrupt and stall once this command (and its data) is done */
    v1.cmd = cmd; v1.num = num; v1.imm = imm; v1.got = 0; v1.need = 0;
    if ((cmd & 0x60) == 0x60) {   /* UNPACK */
        v1.up_vn = (cmd >> 2) & 3; v1.up_vl = cmd & 3; v1.up_m = (cmd >> 4) & 1;
        v1.up_usn = (imm >> 14) & 1;
        v1.up_addr = (imm & 0x3ff) + ((imm >> 15) & 1 ? v1.tops : 0);
        v1.up_total = num ? num : 256;
        v1.up_cnt = 0; ubn = 0;
        uint32_t cl = v1.cl ? v1.cl : 1, wl = v1.wl ? v1.wl : 1;
        uint32_t elems = v1.up_total;
        if (wl > cl) elems = (v1.up_total / wl) * cl + (v1.up_total % wl < cl ? v1.up_total % wl : cl);
        v1.up_total = elems;
        uint32_t bitsz = (uint32_t)elem_bits() * elems;
        v1.need = (bitsz + 31) / 32;
        if (!v1.need) v1.cmd = 0;
        static int tr2 = -1;
        if (tr2 < 0) tr2 = getenv("HP_VIFTRACE") ? atoi(getenv("HP_VIFTRACE")) : 0;
        if (tr2 > 0) { tr2--; LOG("[vif1] %08x UNPACK vn %u vl %u m %u num %u addr %03x cl %u wl %u -> %u words\n", w, v1.up_vn, v1.up_vl, v1.up_m, num, v1.up_addr, v1.cl, v1.wl, v1.need); }
        return;
    }
    switch (cmd) {
    case 0x00: break;
    case 0x01: v1.cl = imm & 0xff; v1.wl = (imm >> 8) & 0xff; break;
    case 0x02: v1.ofst = imm & 0x3ff; v1.dbf = 0; v1.tops = v1.base; break;
    case 0x03: v1.base = imm & 0x3ff; break;
    case 0x04: v1.itops = imm & 0x3ff; break;
    case 0x05: v1.mode = imm & 3; break;
    case 0x06: break;   /* MSKPATH3 */
    case 0x07: v1.mark = imm; v1.mark_seen = 1; break;
    case 0x10: case 0x11: break;   /* FLUSHE / FLUSH: VU1 and PATH1/2 run synchronously */
    case 0x13: {   /* FLUSHA: also PATH3 */
        extern int dma_path3_busy(void); extern void synclog(const char *, ...);
        if (dma_path3_busy()) path3_wait = 1;
        synclog("vif1 FLUSHA%s", path3_wait ? " (waits for PATH3)" : "");
        break;
    }
    case 0x14: case 0x15: vif1_mscal(imm); break;
    case 0x17: { extern uint32_t vu1_tpc(void); vif1_mscal(vu1_tpc()); break; }   /* MSCNT: resume at the VU's TPC */
    case 0x20: v1.need = 1; break;
    case 0x30: case 0x31: v1.need = 4; break;
    case 0x4a: v1.need = (num ? num : 256) * 2; v1.mpg_addr = imm * 8; break;
    case 0x50: case 0x51: v1.need = (imm ? imm : 65536) * 4; v1.dq_n = 0; break;
    default: {
        static int n;
        if (n++ < 10) LOG("[vif1] unknown command %08x\n", w);
        break;
    }
    }
    if (!v1.need) v1.cmd = 0;
    static int tr = -1;
    if (tr < 0) tr = getenv("HP_VIFTRACE") ? atoi(getenv("HP_VIFTRACE")) : 0;
    if (tr > 0) { tr--; LOG("[vif1] %08x cmd %02x num %u imm %04x -> %u data words\n", w, cmd, num, imm, v1.need); }
}

static void vif1_data(uint32_t w) {
    switch (v1.cmd) {
    case 0x20: v1.mask = w; break;
    case 0x30: v1.row[v1.got] = w; break;
    case 0x31: v1.col[v1.got] = w; break;
    case 0x4a: {
        extern unsigned g_vu1_code_gen, g_vu1_mpg_words;
        uint8_t *dst = g_vu1_code + ((v1.mpg_addr + 4 * v1.got) & 0x3fff);
        if (memcmp(dst, &w, 4)) { memcpy(dst, &w, 4); g_vu1_code_gen++; }
        g_vu1_mpg_words++;
        break;
    }
    case 0x50: case 0x51:
        v1.dq.w[v1.dq_n++] = w;
        if (v1.dq_n == 4) { gif_feed(1, &v1.dq, 1); v1.dq_n = 0; }
        break;
    default:
        if ((v1.cmd & 0x60) == 0x60) {
            int esz = elem_bits() / 8;
            memcpy(ub + ubn, &w, 4);
            ubn += 4;
            while (ubn >= esz && v1.up_cnt < v1.up_total) {
                unpack_element(ub);
                memmove(ub, ub + esz, (size_t)(ubn - esz));
                ubn -= esz;
            }
        }
        break;
    }
    v1.got++;
    if (v1.got >= v1.need) { v1.cmd = 0; v1.need = 0; }
}

static void vif1_exec_word(uint32_t w) {
    g_vif1_words++;
    if (v1.need) vif1_data(w);
    else vif1_command(w);
    if (stall_next && !v1.need) {   /* the i-bit command has finished: stop and tell the EE */
        stall_next = 0;
        stalled = 1;
        { extern void synclog(const char *, ...); synclog("vif1 I-stall code %08x mark %x", v1.code, v1.mark); }
        kern_raise_intc(5);
    }
}

/* Runs words until done or until the VIF stops (i-bit stall, FLUSHA waiting for PATH3); returns
   the words consumed.  Whole UNPACK elements go through unpack_fast. */
static uint32_t vif1_exec_block(const uint32_t *w, uint32_t n) {
    uint32_t i = 0;
    while (i < n && !stalled && !path3_wait) {
        if (v1.need && (v1.cmd & 0x60) == 0x60) {
            uint32_t want = v1.need - v1.got;
            uint32_t k = unpack_fast(w + i, want < n - i ? want : n - i);
            if (k) {
                g_vif1_words += k;
                v1.got += k;
                i += k;
                if (v1.got >= v1.need) { v1.cmd = 0; v1.need = 0; }
                if (stall_next && !v1.need) {
                    stall_next = 0;
                    stalled = 1;
                    { extern void synclog(const char *, ...); synclog("vif1 I-stall code %08x mark %x", v1.code, v1.mark); }
                    kern_raise_intc(5);
                }
                continue;
            }
        }
        vif1_exec_word(w[i++]);
    }
    return i;
}

static inline double vnow(void) { struct timespec t; clock_gettime(CLOCK_UPTIME_RAW, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
extern double g_prof[8];   /* 5: VIF1 command processing including VU1 (vu1.c: 4 = VU1 alone) */
static void vif1_drain_locked(void) {
    double t0 = vnow();
    while (!stalled && !path3_wait && vq_head != vq_tail) {
        uint32_t at = vq_head & (VQ_SIZE - 1), len = vq_tail - vq_head;
        if (len > VQ_SIZE - at) len = VQ_SIZE - at;   /* contiguous part of the ring */
        vq_head += vif1_exec_block(&vq[at], len);
    }
    if (!stalled && !path3_wait && vq_head == vq_tail && dma1_deferred) {
        dma1_deferred = 0;
        extern void dma_complete_vif1(uint32_t qwords);
        dma_complete_vif1(dma1_qwords);
    }
    g_prof[5] += vnow() - t0;
}

/* Render thread, g_gs_lock held: every GIF DMA transfer started so far has gone through. */
void vif1_path3_idle(void) {
    if (!path3_wait) return;
    path3_wait = 0;
    vif1_drain_locked();
}

void vif1_word(uint32_t w) {
    if (!vq) vq = malloc(VQ_SIZE * sizeof(uint32_t));
    if (stalled || path3_wait || vq_head != vq_tail) {
        if (vq_tail - vq_head >= VQ_SIZE) { LOG("[vif1] queue overflow\n"); return; }
        vq[vq_tail & (VQ_SIZE - 1)] = w;
        vq_tail++;
        return;
    }
    vif1_exec_word(w);
}

/* Called by the DMA engine (lock held) once every qword of a VIF1 transfer was handed over. */
int vif1_dma_done(uint32_t qwords) {
    if (!stalled && !path3_wait && vq_head == vq_tail) return 1;   /* fully consumed: complete now */
    dma1_deferred = 1;
    dma1_qwords = qwords;
    return 0;
}

/* Data words straight from the DMA engine (lock held). */
void vif1_words(const uint32_t *w, uint32_t n) {
    if (!vq) vq = malloc(VQ_SIZE * sizeof(uint32_t));
    uint32_t i = 0;
    if (!stalled && !path3_wait && vq_head == vq_tail) i = vif1_exec_block(w, n);   /* timed by the DMA caller (g_prof[0]) */
    for (; i < n; i++) vif1_word(w[i]);   /* the rest waits in the queue */
}

void vif1_qword(const U128 *q, int is_tag) {
    if (is_tag) { vif1_word(q->w[2]); vif1_word(q->w[3]); return; }
    for (int k = 0; k < 4; k++) vif1_word(q->w[k]);
}
