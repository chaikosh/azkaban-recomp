/* DMA controller channels and the VIF/GIF/IPU FIFOs (stubs until the GS work). */
#include "rt.h"
#include <stdlib.h>
#include <pthread.h>
#include "gs.h"
#include <time.h>

typedef void (*DmaSink)(const U128 *q, uint32_t n, int is_tag);
static void dma_run(int ch, DmaSink sink, int tte);
extern pthread_mutex_t g_gs_lock;
static U128 *dma_ptr(uint32_t a);
static void gif_sink(const U128 *q, uint32_t n, int is_tag);
static void ipu_sink(const U128 *q, uint32_t n, int is_tag);
static void vif1_sink(const U128 *q, uint32_t n, int is_tag);
static uint32_t chcr_now(int ch);

static const char *chname[10] = {"VIF0", "VIF1", "GIF", "fromIPU", "toIPU", "SIF0", "SIF1", "SIF2", "fromSPR", "toSPR"};

static const uint32_t base[10] = {0x10008000, 0x10009000, 0x1000a000, 0x1000b000, 0x1000b400,
                                  0x1000c000, 0x1000c400, 0x1000c800, 0x1000d000, 0x1000d400};
extern uint8_t *g_spr;

/* Scratchpad <-> RAM.  fromSPR (8) is always normal mode; toSPR (9) supports
 * normal and source-chain mode. */
static uint8_t *ram_or_spr(uint32_t a) {
    if ((a & 0x80000000u) || (a >> 28) == 7) return g_spr + (a & 0x3ff0);
    return g_ram + (a & 0x01fffff0u);
}

static void spr_transfer(int ch) {
    uint32_t chcr = mmio_r32(base[ch]), madr = mmio_r32(base[ch] + 0x10), qwc = mmio_r32(base[ch] + 0x20) & 0xffff;
    uint32_t sadr = mmio_r32(base[ch] + 0x80) & 0x3ff0;
    int mode = (chcr >> 2) & 3;
    {
        static long tl = -2; if (tl == -2) tl = getenv("HP_TAGLOG") ? atol(getenv("HP_TAGLOG")) : -1;
        extern unsigned long long g_vsyncs;
        if (tl >= 0 && (long)g_vsyncs >= tl && (long)g_vsyncs < tl + 60) {
            uint32_t nz = 0;
            for (uint32_t k = 0; k < qwc; k++) { const uint8_t *q = ch == 8 ? g_spr + ((sadr + 16 * k) & 0x3ff0) : ram_or_spr(madr + 16 * k); uint64_t a, b; memcpy(&a, q, 8); memcpy(&b, q + 8, 8); if (a | b) nz++; }
            LOG("[spr%d v%llu] mode %d madr %08x sadr %04x qwc %u nonzero %u sqwc %08x\n", ch, g_vsyncs, mode, madr, sadr, qwc, nz, mmio_r32(0x1000e030u));
        }
    }
    if (ch == 8 || mode == 0) {
        for (uint32_t k = 0; k < qwc; k++) {
            uint8_t *mem = ram_or_spr(madr + 16 * k), *spr = g_spr + ((sadr + 16 * k) & 0x3ff0);
            if (ch == 8) memcpy(mem, spr, 16); else memcpy(spr, mem, 16);
        }
        return;
    }
    /* toSPR chain: tags in RAM at TADR */
    uint32_t tadr = mmio_r32(base[ch] + 0x30);
    for (int guard = 0; guard < 100000; guard++) {
        uint64_t tag;
        memcpy(&tag, ram_or_spr(tadr), 8);
        uint32_t n = tag & 0xffff, id = (tag >> 28) & 7, addr = (uint32_t)(tag >> 32) & 0x7ffffff0u;
        uint32_t src = (id == 1 || id == 2 || id == 7 || id == 0) ? (id == 0 || id == 7 ? addr : tadr + 16) : addr;
        if (id == 3 || id == 4) src = addr;
        for (uint32_t k = 0; k < n; k++) { memcpy(g_spr + (sadr & 0x3ff0), ram_or_spr(src + 16 * k), 16); sadr += 16; }
        if (id == 1) tadr = tadr + 16 + 16 * n;
        else if (id == 2) tadr = addr;
        else if (id == 0 || id == 7) break;
        else tadr += 16;
        if ((tag >> 31) & 1) break;   /* IRQ bit with TIE ends the chain */
    }
}

unsigned g_dma_count[10];
static unsigned g_dma_qwords;
void dma_finish(int ch);
void kern_after(uint64_t ns, void (*fn)(int), int arg);

void dma_stats(void) {
    static unsigned last[10];
    LOG("[stats] dma/2s:");
    for (int k = 0; k < 10; k++) { LOG(" %s=%u", chname[k], g_dma_count[k] - last[k]); last[k] = g_dma_count[k]; }
    LOG("\n");
    extern unsigned long long g_gs_prims, g_gs_pixels, g_vu1_insns, g_vif1_words;
    extern GSState gs;
    static unsigned long long lp, lpx, lvu, lvw;
    extern unsigned g_xfer_count[3];
    extern unsigned g_prims_by_path[3];
    LOG("[stats] gs xfers path1 %u path2 %u path3 %u, prims path1 %u path2 %u path3 %u\n", g_xfer_count[0], g_xfer_count[1], g_xfer_count[2],
        g_prims_by_path[0], g_prims_by_path[1], g_prims_by_path[2]);
    LOG("[stats] gs prims %llu pixels %llu, vu1 insns %llu, vif1 words %llu, FRAME1 %llx FRAME2 %llx DISPFB1 %llx DISPFB2 %llx PMODE %llx\n",
        g_gs_prims - lp, g_gs_pixels - lpx, g_vu1_insns - lvu, g_vif1_words - lvw,
        (unsigned long long)gs.frame[0], (unsigned long long)gs.frame[1],
        (unsigned long long)hw_gs_priv(0x70), (unsigned long long)hw_gs_priv(0x90), (unsigned long long)hw_gs_priv(0));
    lp = g_gs_prims; lpx = g_gs_pixels; lvu = g_vu1_insns; lvw = g_vif1_words;
    extern unsigned g_snd_acks, g_snd_updates;
    LOG("[stats] snd updates %u acks %u\n", g_snd_updates, g_snd_acks);
    { extern unsigned long long g_fifo1_qw, g_fifo1_nz; LOG("[stats] vif1 fifo qwords %llu nonzero %llu\n", g_fifo1_qw, g_fifo1_nz); }
    extern void vif_stats(void), gs_fb_stats(void), ipu_stats(void);
    vif_stats();
    gs_fb_stats();
    ipu_stats();
}

/* VIF1 and GIF transfers run on a render thread, like the real DMA/VU/GS
 * running beside the CPU: the game keeps going while CHCR.STR is set. */
pthread_mutex_t g_gs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t rq_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rq_cv = PTHREAD_COND_INITIALIZER;
static int rq[32], rq_head, rq_tail, rthread_started;

static void render_channel(int ch) {
    extern double g_prof[8];
    struct timespec pa, pb;
    clock_gettime(CLOCK_MONOTONIC, &pa);
    pthread_mutex_lock(&g_gs_lock);
    g_dma_qwords = 0;
    if (ch == 1) {
        if (g_trace > 1) LOG("[dma1] start CHCR %08x MADR %08x QWC %u TADR %08x\n", mmio_r32(base[1]), mmio_r32(base[1] + 0x10),
                             mmio_r32(base[1] + 0x20), mmio_r32(base[1] + 0x30));
        dma_run(1, vif1_sink, (chcr_now(1) >> 6) & 1);
    } else dma_run(2, gif_sink, 0);
    uint32_t q = g_dma_qwords;
    extern int vif1_dma_done(uint32_t qwords);
    int complete = ch == 1 ? vif1_dma_done(q) : 1;
    pthread_mutex_unlock(&g_gs_lock);
    clock_gettime(CLOCK_MONOTONIC, &pb);
    if (ch == 1) g_prof[0] += (pb.tv_sec - pa.tv_sec) + (pb.tv_nsec - pa.tv_nsec) * 1e-9;
    if (complete) kern_after(10000 + (uint64_t)q * 8, dma_finish, ch);
}

static void *render_thread(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&rq_m);
        while (rq_head == rq_tail) pthread_cond_wait(&rq_cv, &rq_m);
        int ch = rq[rq_head];
        rq_head = (rq_head + 1) % 32;
        pthread_mutex_unlock(&rq_m);
        render_channel(ch);
    }
    return NULL;
}

void dma_start(int ch) {
    static int seen[10];
    g_dma_count[ch]++;
    if (!seen[ch]++) LOG("[dma] first transfer on %s\n", chname[ch]);
    if (ch == 5) return;   /* SIF0 is armed by the game and fed by the IOP */
    if ((ch == 3 || ch == 4) && g_trace) {
        uint32_t madr = mmio_r32(base[ch] + 0x10), qwc = mmio_r32(base[ch] + 0x20);
        const uint8_t *p = (const uint8_t *)dma_ptr(madr);
        LOG("[ipu] dma %s CHCR %08x MADR %08x QWC %u data %02x%02x%02x%02x %02x%02x%02x%02x\n", chname[ch], mmio_r32(base[ch]), madr, qwc,
            p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
    }
    if (ch == 1 || ch == 2) {
        pthread_mutex_lock(&rq_m);
        if (!rthread_started) {
            pthread_t t;
            pthread_attr_t at;
            pthread_attr_init(&at);
            pthread_attr_setstacksize(&at, 16u << 20);
            pthread_create(&t, &at, render_thread, NULL);
            rthread_started = 1;
        }
        rq[rq_tail] = ch;
        rq_tail = (rq_tail + 1) % 32;
        pthread_cond_signal(&rq_cv);
        pthread_mutex_unlock(&rq_m);
        return;
    }
    g_dma_qwords = 0;
    if (ch == 8 || ch == 9) spr_transfer(ch);
    if (ch == 4) dma_run(4, ipu_sink, 0);
    if (ch == 3) {   /* fromIPU completes when the decoder has produced QWC quadwords */
        extern void ipu_from_dma_start(uint32_t madr, uint32_t qwc);
        ipu_from_dma_start(mmio_r32(base[3] + 0x10), mmio_r32(base[3] + 0x20) & 0xffff);
        return;
    }
    kern_after(20000 + (uint64_t)g_dma_qwords * 8, dma_finish, ch);
}

void dma_finish(int ch) {
    extern void mmio_w32_raw(uint32_t a, uint32_t v);
    uint32_t chcr = mmio_r32(base[ch]);
    mmio_w32_raw(base[ch], chcr & ~0x100u);
    mmio_w32_raw(base[ch] + 0x20, 0);
    kern_raise_dmac(ch);
}

/* Source-chain DMA: walks DMAtags and hands each data run to `sink`.
 * `tte` also passes the upper half of each tag (VIF channels). */

static U128 *dma_ptr(uint32_t a) {
    if ((a & 0x80000000u) || (a >> 28) == 7) return (U128 *)(g_spr + (a & 0x3ff0));
    return (U128 *)(g_ram + (a & 0x01fffff0u));
}

static void dma_run(int ch, DmaSink sink, int tte) {
    uint32_t b = base[ch];
    uint32_t chcr = mmio_r32(b), madr = mmio_r32(b + 0x10), qwc = mmio_r32(b + 0x20) & 0xffff;
    int mode = (chcr >> 2) & 3;
    if (mode == 0 || qwc) {
        /* normal transfer, also the pending QWC of a chain before its tags */
        {
            static long tl = -2; if (tl == -2) tl = getenv("HP_TAGLOG") ? atol(getenv("HP_TAGLOG")) : -1;
            extern unsigned long long g_vsyncs;
            if (ch == 1 && tl >= 0 && (long)g_vsyncs >= tl && (long)g_vsyncs < tl + 60) {
                uint32_t nz = 0;
                for (uint32_t k = 0; k < qwc; k++) { const U128 *q = dma_ptr(madr + 16 * k); if (q->d[0] | q->d[1]) nz++; }
                LOG("[dma1n v%llu] mode %d madr %08x qwc %u nonzero %u first %016llx %016llx\n", g_vsyncs, mode, madr, qwc, nz,
                    (unsigned long long)dma_ptr(madr)->d[0], (unsigned long long)dma_ptr(madr)->d[1]);
            }
        }
        for (uint32_t k = 0; k < qwc; k++) sink(dma_ptr(madr + 16 * k), 1, 0);
        if (mode == 0) return;
    }
    uint32_t tadr = mmio_r32(b + 0x30);
    uint32_t asr[2] = {mmio_r32(b + 0x40), mmio_r32(b + 0x50)};
    int asp = (chcr >> 4) & 3;
    for (int guard = 0; guard < 1000000; guard++) {
        U128 *tq = dma_ptr(tadr);
        uint64_t tag = tq->d[0];
        uint32_t n = tag & 0xffff, id = (tag >> 28) & 7, addr = (uint32_t)(tag >> 32) & 0xfffffff0u;
        if (tte) sink(tq, 1, 1);
        int end = 0;
        uint32_t data, next;
        switch (id) {
        case 0: data = addr; next = 0; end = 1; break;                       /* refe */
        case 1: data = tadr + 16; next = tadr + 16 + 16 * n; break;          /* cnt */
        case 2: data = tadr + 16; next = addr; break;                        /* next */
        case 3: case 4: data = addr; next = tadr + 16; break;                /* ref, refs */
        case 5:                                                              /* call */
            data = tadr + 16;
            if (asp < 2) asr[asp++] = tadr + 16 + 16 * n;
            next = addr;
            break;
        case 6:                                                              /* ret */
            data = tadr + 16;
            if (asp > 0) next = asr[--asp]; else { next = 0; end = 1; }
            break;
        default: data = tadr + 16; next = 0; end = 1; break;                 /* end */
        }
        {   /* HP_TAGLOG=<vsync>: log channel-1 chain tags for 60 vsyncs from there */
            static long tl = -2; if (tl == -2) tl = getenv("HP_TAGLOG") ? atol(getenv("HP_TAGLOG")) : -1;
            extern unsigned long long g_vsyncs;
            if ((ch == 1 || ch == 2) && tl >= 0 && (long)g_vsyncs >= tl && (long)g_vsyncs < tl + 60) {
                const U128 *d0 = n ? dma_ptr(data) : NULL;
                uint32_t nz = 0;
                for (uint32_t k = 0; k < n; k++) { const U128 *q = dma_ptr(data + 16 * k); if (q->d[0] | q->d[1]) nz++; }
                LOG("[tag%d v%llu] tadr %08x id %u n %u addr %08x data %08x nonzero %u first %016llx\n", ch, g_vsyncs, tadr, id, n, addr, data, nz,
                    d0 ? (unsigned long long)d0->d[0] : 0ull);
            }
        }
        for (uint32_t k = 0; k < n; k++) sink(dma_ptr(data + 16 * k), 1, 0);
        if (end) break;
        tadr = next;
        if (((tag >> 31) & 1) && (chcr & 0x80)) break;   /* IRQ with TIE stops the chain */
    }
    extern void mmio_w32_raw(uint32_t a, uint32_t v);
    mmio_w32_raw(b + 0x30, tadr);
    if (g_trace > 1 && ch == 1) {
        U128 *t = dma_ptr(tadr);
        LOG("[dma1] chain ended, TADR %08x tag %016llx\n", tadr, (unsigned long long)t->d[0]);
    }
}

static uint32_t chcr_now(int ch) { return mmio_r32(base[ch]); }
void vif1_qword(const U128 *q, int is_tag);
static void vif1_sink(const U128 *q, uint32_t n, int is_tag) {
    g_dma_qwords += n;
    for (uint32_t k = 0; k < n; k++) vif1_qword(&q[k], is_tag);
}

static void ipu_sink(const U128 *q, uint32_t n, int is_tag) {
    extern void ipu_in_qwords(const uint8_t *p, uint32_t nq);
    g_dma_qwords += n;
    if (!is_tag) ipu_in_qwords((const uint8_t *)q, n);
}

static void gif_sink(const U128 *q, uint32_t n, int is_tag) {
    extern void gif_feed(int path, const U128 *q, uint32_t n);
    g_dma_qwords += n;
    if (!is_tag) gif_feed(2, q, n);
}

unsigned long long g_fifo1_qw, g_fifo1_nz;
void fifo_write(int which, U128 v) {
    if (which == 1) { g_fifo1_qw++; if (v.d[0] | v.d[1]) g_fifo1_nz++; }
    extern void gif_feed(int path, const U128 *q, uint32_t n);
    if (which == 2) { pthread_mutex_lock(&g_gs_lock); gif_feed(2, &v, 1); pthread_mutex_unlock(&g_gs_lock); return; }
    if (which == 1) { pthread_mutex_lock(&g_gs_lock); vif1_qword(&v, 0); pthread_mutex_unlock(&g_gs_lock); return; }
    if (which == 3) { extern void ipu_in_qwords(const uint8_t *p, uint32_t nq); ipu_in_qwords((const uint8_t *)&v, 1); return; }
    static int seen[4];
    if (!seen[which]++) LOG("[fifo] first write to FIFO %d\n", which);
}

void gs_priv_write(uint32_t off, uint64_t v) {
    if (g_trace) LOG("[gs] priv %04x = %016llx\n", off, (unsigned long long)v);
}
