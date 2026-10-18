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
uint32_t vif1_reg_read(uint32_t off) {
    switch (off) {   /* register map per the EE manual: STAT 00 FBRST 10 ERR 20 MARK 30 CYCLE 40 MODE 50 NUM 60 MASK 70 CODE 80 ... */
    case 0x00: return (stalled ? 0xc00 : 0) | (v1.mark_seen ? 0x40 : 0);   /* VIS+INT while stalled by an i bit, MRK */
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

void vif1_reg_write(uint32_t off, uint32_t val) {
    if (off == 0x30) { v1.mark = val; v1.mark_seen = 0; }   /* writing MARK clears MRK */
    else if (off == 0x10) {
        pthread_mutex_lock(&g_gs_lock);
        if (val & 1) { v1.need = 0; v1.cmd = 0; stalled = stall_next = 0; vq_head = vq_tail = 0; }   /* RST */
        if (val & 8) { stalled = 0; vif1_drain_locked(); }                                          /* STC */
        pthread_mutex_unlock(&g_gs_lock);
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

static void unpack_element(const uint8_t *e) {
    uint32_t c[4];
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

static void vif1_mscal(uint32_t addr_pairs) {
    v1.top = v1.tops;
    v1.itop = v1.itops;
    /* double buffering */
    v1.tops = v1.dbf ? v1.base : v1.base + v1.ofst;
    v1.dbf ^= 1;
    v1.last_pc = addr_pairs;
    vu1_run(addr_pairs, v1.top, v1.itop);
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
    case 0x10: case 0x11: case 0x13: break;   /* FLUSHE / FLUSH / FLUSHA: VU runs synchronously */
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
    case 0x4a:
        memcpy(g_vu1_code + ((v1.mpg_addr + 4 * v1.got) & 0x3fff), &w, 4);
        break;
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
        kern_raise_intc(5);
    }
}

static void vif1_drain_locked(void) {
    while (!stalled && vq_head != vq_tail) {
        uint32_t w = vq[vq_head & (VQ_SIZE - 1)];
        vq_head++;
        vif1_exec_word(w);
    }
    if (!stalled && vq_head == vq_tail && dma1_deferred) {
        dma1_deferred = 0;
        kern_after(10000 + (uint64_t)dma1_qwords * 8, dma_finish, 1);
    }
}

void vif1_word(uint32_t w) {
    if (!vq) vq = malloc(VQ_SIZE * sizeof(uint32_t));
    if (stalled || vq_head != vq_tail) {
        if (vq_tail - vq_head >= VQ_SIZE) { LOG("[vif1] queue overflow\n"); return; }
        vq[vq_tail & (VQ_SIZE - 1)] = w;
        vq_tail++;
        return;
    }
    vif1_exec_word(w);
}

/* Called by the DMA engine (lock held) once every qword of a VIF1 transfer was handed over. */
int vif1_dma_done(uint32_t qwords) {
    if (!stalled && vq_head == vq_tail) return 1;   /* fully consumed: complete now */
    dma1_deferred = 1;
    dma1_qwords = qwords;
    return 0;
}

void vif1_qword(const U128 *q, int is_tag) {
    if (is_tag) { vif1_word(q->w[2]); vif1_word(q->w[3]); return; }
    for (int k = 0; k < 4; k++) vif1_word(q->w[k]);
}
