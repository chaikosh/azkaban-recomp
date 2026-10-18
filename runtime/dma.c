/* DMA controller channels and the VIF/GIF/IPU FIFOs (stubs until the GS work). */
#include "rt.h"
void hp_thread_register(const char *name);
void hp_thread_report(double wall);
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
        { static int nl; if (getenv("HP_SPRTAGS") && nl < 400) { nl++; LOG("[spr9 tag] chcr %08x tadr %08x id %u n %u addr %08x irq %u sadr %04x\n", chcr, tadr, id, n, addr, (unsigned)((tag >> 31) & 1), sadr); } }
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
static void dma_complete_after(uint64_t base_ns, uint64_t qwords, int ch);

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
    {   /* speed: game frames and render-thread time per 2 s (wall) */
        extern unsigned g_frames_queued; extern double g_prof[8];
        static unsigned lf; static double lp[8]; static struct timespec lt; static unsigned long long lvs; extern unsigned long long g_vsyncs;
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        double wall = lt.tv_sec ? (now.tv_sec - lt.tv_sec) + (now.tv_nsec - lt.tv_nsec) * 1e-9 : 2.0;
        LOG("[speed] %.1f game fps | vu1 %.0f%% | render %.0f%% (vif1 %.0f%% tri %.0f%% sprite %.0f%% wait %.0f%%)\n", (g_frames_queued - lf) / wall, 100 * (g_prof[4] - lp[4]) / wall,
            100 * (g_prof[0] - lp[0]) / wall, 100 * (g_prof[0] - lp[0] - (g_prof[2] - lp[2]) - (g_prof[1] - lp[1])) / wall,
            100 * (g_prof[2] - lp[2]) / wall, 100 * (g_prof[1] - lp[1]) / wall, 100 * (g_prof[3] - lp[3]) / wall);
        lf = g_frames_queued; for (int k = 0; k < 8; k++) lp[k] = g_prof[k]; lt = now;
        hp_thread_report(wall);
        { extern int g_gs_gpu; extern void gsb_times(double *, double *, double *);
          static double lg, ld, ldl;
          if (g_gs_gpu) { double g, d, dl; gsb_times(&g, &d, &dl);
            LOG("[gpu] gif %.0f%% display %.0f%% download %.0f%% of wall, %.1f vsyncs/s\n", 100 * (g - lg) / wall, 100 * (d - ld) / wall, 100 * (dl - ldl) / wall,
                (double)(g_vsyncs - lvs) / wall);
            lg = g; ld = d; ldl = dl; }
          lvs = g_vsyncs; }
        { extern unsigned long long g_ev_count, g_ev_late_ns; static unsigned long long lc, ll;
          unsigned long long n = g_ev_count - lc, l = g_ev_late_ns - ll; lc = g_ev_count; ll = g_ev_late_ns;
          LOG("[events] %.0f/s, mean lateness %.0f us, total late %.0f%% of wall\n", n / wall, n ? l / 1000.0 / n : 0.0, 100.0 * l / 1e9 / wall); }
    }
    { extern unsigned long long g_fifo1_qw, g_fifo1_nz; LOG("[stats] vif1 fifo qwords %llu nonzero %llu\n", g_fifo1_qw, g_fifo1_nz); }
    extern void vif_stats(void), gs_fb_stats(void), ipu_stats(void);
    vif_stats();
    if (getenv("HP_VUSTATS")) { extern void vu1_stats(void); vu1_stats(); }
    { extern void vu1rec_stats(void); vu1rec_stats(); }
    { extern void mmio_stats_report(void); mmio_stats_report(); }
    gs_fb_stats();
    ipu_stats();
}

/* VIF1 and GIF transfers run on a render thread, like the real DMA/VU/GS
 * running beside the CPU: the game keeps going while CHCR.STR is set. */
pthread_mutex_t g_gs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t rq_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rq_cv = PTHREAD_COND_INITIALIZER;
static int rq[32], rq_head, rq_tail, rthread_started;

/* GIF DMA transfers started but not yet run by the render thread: PATH3 is busy (VIF1 FLUSHA). */
static int path3_pending;
int dma_path3_busy(void) { return __atomic_load_n(&path3_pending, __ATOMIC_SEQ_CST) > 0; }

static void render_channel(int ch) {
    extern double g_prof[8];
    struct timespec pa, pb;
    clock_gettime(CLOCK_MONOTONIC, &pa);
    pthread_mutex_lock(&g_gs_lock);
    g_dma_qwords = 0;
    if (ch == 1 && !(chcr_now(1) & 1)) {   /* VIF1 to memory: a GS local->host download, ordered after earlier GS work */
        extern void gs_read_image(uint8_t *dst, uint32_t qwords);
        extern void mmio_w32_raw(uint32_t a, uint32_t v);
        uint32_t madr = mmio_r32(base[1] + 0x10), qwc = mmio_r32(base[1] + 0x20) & 0xffff;
        for (uint32_t k = 0; k < qwc; k++) gs_read_image((uint8_t *)dma_ptr(madr + 16 * k), 1);
        mmio_w32_raw(base[1] + 0x10, madr + 16 * qwc);
        pthread_mutex_unlock(&g_gs_lock);
        dma_complete_after(20000, qwc, 1);
        return;
    }
    if (ch == 1) {
        if (g_trace > 1) LOG("[dma1] start CHCR %08x MADR %08x QWC %u TADR %08x\n", mmio_r32(base[1]), mmio_r32(base[1] + 0x10),
                             mmio_r32(base[1] + 0x20), mmio_r32(base[1] + 0x30));
        dma_run(1, vif1_sink, (chcr_now(1) >> 6) & 1);
    } else {
        dma_run(2, gif_sink, 0);
        if (__atomic_sub_fetch(&path3_pending, 1, __ATOMIC_SEQ_CST) == 0) { extern void vif1_path3_idle(void); vif1_path3_idle(); }
    }
    uint32_t q = g_dma_qwords;
    extern int vif1_dma_done(uint32_t qwords);
    int complete = ch == 1 ? vif1_dma_done(q) : 1;
    pthread_mutex_unlock(&g_gs_lock);
    clock_gettime(CLOCK_MONOTONIC, &pb);
    if (ch == 1) g_prof[0] += (pb.tv_sec - pa.tv_sec) + (pb.tv_nsec - pa.tv_nsec) * 1e-9;
    if (complete) dma_complete_after(10000, q, ch);
}

/* Render work runs one item at a time, in order: on the render thread, or (GIF transfers) right
   away on the posting thread when nothing is queued or running: the game busy-waits on D2_CHCR
   after each GIF transfer, and waking the render thread costs more than the work.
   HP_RENDERTHREAD=1 always uses the render thread. */
static int r_busy;
static void run_job(int job) {
    if (job == 16) { extern void vif1_drain_job(void); vif1_drain_job(); }
    else render_channel(job);
}
static void *render_thread(void *arg) {
    hp_thread_register("render");
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&rq_m);
        while (rq_head == rq_tail || r_busy) pthread_cond_wait(&rq_cv, &rq_m);
        int ch = rq[rq_head];
        rq_head = (rq_head + 1) % 32;
        r_busy = 1;
        pthread_mutex_unlock(&rq_m);
        run_job(ch);
        pthread_mutex_lock(&rq_m);
        r_busy = 0;
        pthread_cond_broadcast(&rq_cv);
        pthread_mutex_unlock(&rq_m);
    }
    return NULL;
}

/* Queue work for the render thread: a DMA channel (1, 2) or a job (16 = resume VIF1 after STC). */
void render_post(int job) {
    static int threaded = -1;
    if (threaded < 0) threaded = getenv("HP_RENDERTHREAD") != NULL;
    pthread_mutex_lock(&rq_m);
    if (!rthread_started) {
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 16u << 20);
        pthread_create(&t, &at, render_thread, NULL);
        rthread_started = 1;
    }
    /* only GIF (2): short transfers the EE waits for right away; VIF1 (1) stays on the render
       thread, where it overlaps with the game's own work */
    if (!threaded && job == 2 && rq_head == rq_tail && !r_busy) {
        r_busy = 1;
        pthread_mutex_unlock(&rq_m);
        run_job(job);
        pthread_mutex_lock(&rq_m);
        r_busy = 0;
        pthread_cond_broadcast(&rq_cv);   /* anything queued meanwhile goes to the render thread */
        pthread_mutex_unlock(&rq_m);
        return;
    }
    if ((rq_tail + 1) % 32 == rq_head) LOG("[dma] render queue full\n");
    rq[rq_tail] = job;
    rq_tail = (rq_tail + 1) % 32;
    pthread_cond_broadcast(&rq_cv);
    pthread_mutex_unlock(&rq_m);
}

static int d4_end;
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
    if (ch == 2) __atomic_add_fetch(&path3_pending, 1, __ATOMIC_SEQ_CST);
    if (ch == 1 || ch == 2) { render_post(ch); return; }
    g_dma_qwords = 0;
    if (ch == 8 || ch == 9) spr_transfer(ch);
    if (ch == 4) {   /* toIPU is pulled by the IPU as its input FIFO drains */
        extern void ipu_in_kick(void);
        d4_end = 0;
        ipu_in_kick();
        return;
    }
    if (ch == 3) {   /* fromIPU completes when the decoder has produced QWC quadwords */
        extern void ipu_from_dma_start(uint32_t madr, uint32_t qwc);
        ipu_from_dma_start(mmio_r32(base[3] + 0x10), mmio_r32(base[3] + 0x20) & 0xffff);
        return;
    }
    dma_complete_after(20000, g_dma_qwords, ch);
}

/* toIPU pull: hand the IPU one quadword, advancing MADR/QWC/TADR like the DMAC would.
   Returns 0 when the channel is idle or its chain has ended (the channel is then finished). */
int dma_ipu_pull(U128 *q) {
    extern void mmio_w32_raw(uint32_t a, uint32_t v);
    uint32_t b = base[4];
    for (int guard = 0; guard < 4096; guard++) {
        uint32_t chcr = mmio_r32(b);
        if (!(chcr & 0x100)) return 0;
        uint32_t madr = mmio_r32(b + 0x10), qwc = mmio_r32(b + 0x20) & 0xffff;
        int mode = (chcr >> 2) & 3;
        if (qwc) {
            *q = *dma_ptr(madr);
            mmio_w32_raw(b + 0x10, madr + 16);
            mmio_w32_raw(b + 0x20, qwc - 1);
            g_dma_qwords++;
            if (qwc == 1 && (mode == 0 || d4_end)) dma_finish(4);
            return 1;
        }
        if (mode == 0 || d4_end) { dma_finish(4); return 0; }
        /* source chain: fetch the next tag */
        uint32_t tadr = mmio_r32(b + 0x30);
        uint64_t tag = dma_ptr(tadr)->d[0];
        uint32_t n = tag & 0xffff, id = (tag >> 28) & 7, addr = (uint32_t)(tag >> 32) & 0xfffffff0u, data, next;
        switch (id) {
        case 0: data = addr; next = tadr + 16; d4_end = 1; break;          /* refe */
        case 1: data = tadr + 16; next = tadr + 16 + 16 * n; break;        /* cnt */
        case 2: data = tadr + 16; next = addr; break;                      /* next */
        case 3: case 4: data = addr; next = tadr + 16; break;              /* ref, refs */
        default: data = tadr + 16; next = tadr + 16 + 16 * n; d4_end = 1; break;   /* end (call/ret unused here) */
        }
        if (((tag >> 31) & 1) && (chcr & 0x80)) d4_end = 1;               /* IRQ with TIE */
        mmio_w32_raw(b, (chcr & 0xffff) | (uint32_t)(tag & 0xffff0000u));
        mmio_w32_raw(b + 0x10, data);
        mmio_w32_raw(b + 0x20, n);
        mmio_w32_raw(b + 0x30, next);
    }
    return 0;
}

/* Completion latency of the DMA channels that do not model their own timing (SPR, GIF, VIF1).
   HP_DMALAT=base_ns,per_qword_ns; the default 0,0 completes the channel as soon as its data has
   been processed (a timed completion goes through the alarm thread, which itself adds ~10 us, and
   the game busy-waits on CHCR for every transfer). */
static void dma_complete_after(uint64_t base_ns, uint64_t qwords, int ch) {
    static int init; static uint64_t b0, pq;
    if (!init) {
        init = 1;
        const char *e = getenv("HP_DMALAT");
        if (e) { unsigned long long x = 0, y = 0; sscanf(e, "%llu,%llu", &x, &y); b0 = x; pq = y; }
    }
    (void)base_ns;
    uint64_t ns = b0 + qwords * pq;
    if (!ns) dma_finish(ch);
    else kern_after(ns, dma_finish, ch);
}

void dma_complete_vif1(uint32_t qwords) { dma_complete_after(10000, qwords, 1); }

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
