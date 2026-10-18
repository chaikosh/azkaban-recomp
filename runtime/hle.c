/* Host replacements for individual guest functions (see recomp/hooks.txt). */
#include "rt.h"
#include "funcs.h"
#include <stdlib.h>

/* EA stream status: report the IOP-side stream as finished until the sound
 * driver itself runs; otherwise file loading waits for it forever. */
void hle_stream_done(EE *c) {
    R(2) = 1;
}

/* ---- temporary tracing of the frame DMA queue ---- */
#include <time.h>
static double tnow(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static int qlog(void) { static int n = -1; if (n < 0) n = getenv("HP_QLOG") ? 100000 : 0; return n > 0 ? n-- : 0; }
unsigned g_frames_queued;
void hle_log_queue(EE *c) {
    g_frames_queued++;
    if (qlog()) LOG("[q %.4f] queue frame (busy %08x head %08x)\n", tnow(), ld32(0x4df7cc), ld32(0x47b82c));
    f_0013c150_orig(c);
}
void hle_log_kick(EE *c) {
    if (qlog()) LOG("[q %.4f] timer kick head %08x tadr %08x state %u\n", tnow(), ld32(0x47b82c), ld32(0x47b82c) ? ld32(ld32(0x47b82c)) : 0, ld32(0x47b820));
    f_0013dce0_orig(c);
}
void hle_log_done(EE *c) {
    if (qlog()) LOG("[q %.4f] dma done state %u busy %08x TADR %08x\n", tnow(), ld32(0x47b820), ld32(0x4df7cc), ld32(0x10009030));
    f_0013dd48_orig(c);
    if (qlog()) LOG("[q %.4f]   -> state %u busy %08x head %08x\n", tnow(), ld32(0x47b820), ld32(0x4df7cc), ld32(0x47b82c));
}

/* GIF DMA kick (texture uploads and other path-3 packets) */
void hle_log_gifkick(EE *c) {
    static int n;
    uint32_t obj = RW(4), tadr = ld32(obj + 4);
    if (getenv("HP_GIFLOG") && n++ < 400)
        LOG("[gifkick %.3f] obj %08x tadr %08x tag %08x %08x | %08x %08x\n", tnow(), obj, tadr, ld32(tadr), ld32(tadr + 4), ld32(tadr + 8), ld32(tadr + 12));
    f_00143608_orig(c);
}

/* VIF1 interrupt handler: logs the MARK value it dispatches on */
void hle_log_vifirq(EE *c) {
    static int n;
    extern uint32_t vif1_reg_read(uint32_t off);
    if (getenv("HP_VIFLOG") && n++ < 300)
        LOG("[vifirq %.3f] mark %04x stat %03x\n", tnow(), vif1_reg_read(0x30), vif1_reg_read(0));
    f_0013d9f8_orig(c);
}

void hle_log_addcmd(EE *c) {
    extern unsigned long long g_vsyncs;
    if (g_trace) LOG("[sif v%llu] AddCmdHandler cid %08x handler %08x data %08x\n", g_vsyncs, RW(4), RW(5), RW(6));
    f_0044b210_orig(c);
}

void hle_log_sndcmd(EE *c) {
    extern unsigned long long g_vsyncs;
    uint32_t a = RW(4);
    if (g_trace) LOG("[snd v%llu] cmd3 handler pkt %08x: %08x %08x %08x %08x %08x %08x data %08x\n", g_vsyncs, a,
        ld32(a), ld32(a + 4), ld32(a + 8), ld32(a + 12), ld32(a + 16), ld32(a + 20), RW(5));
    f_0032d120_orig(c);
    if (g_trace) LOG("[snd] cmd3 handler done: flag %08x\n", ld32(0x484470u));
}

void hle_log_fopen(EE *c) {
    extern unsigned long long g_vsyncs;
    static int n = 0;
    char name[96]; uint32_t a = RW(4);
    for (int i = 0; i < 95; i++) { name[i] = (char)ld8(a + i); if (!name[i]) break; name[95] = 0; }
    uint32_t a1 = RW(5), a2 = RW(6), a3 = RW(7);
    if (g_trace && n < 400) LOG("[fopen+ v%llu] %s\n", g_vsyncs, name);
    f_002bd678_orig(c);
    if (g_trace && n < 400) { n++; LOG("[fopen v%llu] %s (%08x %08x %08x) -> %d\n", g_vsyncs, name, a1, a2, a3, (int)RW(2)); }
}

void hle_log_stream(EE *c) {
    extern unsigned long long g_vsyncs;
    static uint32_t last[4]; static int n = 0;
    uint32_t h = RW(4);
    f_00304930_orig(c);
    /* the original leaves {obj, aux} on its (now popped) stack frame at sp-0x20 */
    uint32_t sp = RW(29) - 0x20, obj = ld32(sp), aux = ld32(sp + 4);
    uint32_t v[4] = { obj ? ld32(obj + 56) : 0xdead, aux ? ld32(aux + 8) : 0xdead, RW(2), h };
    static int calls = 0; calls++;
    if (g_trace && n < 300 && (memcmp(v, last, sizeof v) || calls % 50 == 0)) { n++; memcpy(last, v, sizeof v);
        LOG("[stream v%llu] handle %08x obj %08x +56=%08x aux %08x +8=%08x -> %u\n", g_vsyncs, h, obj, v[0], aux, v[1], (unsigned)RW(2)); }
}

static void guest_str(uint32_t a, char *out, int n) { for (int i = 0; i < n - 1; i++) { out[i] = (char)ld8(a + i); if (!out[i]) return; } out[n - 1] = 0; }
void hle_log_lsload(EE *c) {
    extern unsigned long long g_vsyncs;
    char name[96]; guest_str(RW(5), name, sizeof name);
    uint32_t obj = RW(4), st0 = ld32(obj + 12);
    f_002c06f0_orig(c);
    if (g_trace) LOG("[ls v%llu tick %u] load '%s' arg %08x: state %u -> %u\n", g_vsyncs, ld32(0x483390u), name, RW(6), st0, ld32(obj + 12));
}
void hle_log_lssusp(EE *c) {
    extern unsigned long long g_vsyncs;
    uint32_t obj = RW(4), st0 = ld32(obj + 12), ra = RW(31);
    f_002c0ef8_orig(c);
    if (g_trace) LOG("[ls v%llu tick %u] suspend (ra %08x): state %u -> %u\n", g_vsyncs, ld32(0x483390u), ra, st0, ld32(obj + 12));
}
void hle_log_lsresume(EE *c) {
    extern unsigned long long g_vsyncs;
    uint32_t obj = RW(4), st0 = ld32(obj + 12), ra = RW(31);
    f_002c0f48_orig(c);
    if (g_trace) LOG("[ls v%llu tick %u] resume (ra %08x): state %u -> %u\n", g_vsyncs, ld32(0x483390u), ra, st0, ld32(obj + 12));
}

/* f_00467230: "no package load pending" (state < 2).  f_002bd678 spins on it before opening a file
   from disc.  When the loader is suspended mid-load (state 2) nothing can change while the caller
   spins, so treat that as "not pending" too; the real console never seems to hit this ordering. */
void hle_loader_idle(EE *c) {
    uint32_t st = ld32(RW(4) + 12);
    R(2) = SX(st != 3 ? 1u : 0u);
}

static void log_pic(const char *tag, uint32_t p) {
    extern unsigned long long g_vsyncs;
    if (!g_trace) return;
    LOG("[mp v%llu] %s pic %08x:", g_vsyncs, tag, p);
    for (int k = 0; k < 26 && p; k++) LOG(" %x", ld32(p + 4 * k));
    LOG("\n");
}
void hle_log_mpout1(EE *c) { log_pic("out1", RW(5)); if (g_trace) LOG("[mp] dec %08x +8=%u +176=%u +372=%u +336=%u\n", RW(4), ld32(RW(4) + 8), ld32(RW(4) + 176), ld32(RW(4) + 372), ld32(RW(4) + 336)); f_002c8640_orig(c); }
void hle_log_mpout2(EE *c) { log_pic("out2a", RW(5)); log_pic("out2b", RW(6)); f_002c8750_orig(c); }
void hle_log_mpcsc(EE *c) {
    log_pic("csc", RW(5));
    {   /* HP_MPDUMP: also save the RGB output of the previous conversion (destination = dec+216) */
        static uint32_t last_dst; static int k;
        const char *dir = getenv("HP_MPDUMP");
        if (dir && last_dst && k < 6) {
            char path[512]; snprintf(path, sizeof path, "%s/rgb%d.bin", dir, k++);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(g_ram + (last_dst & 0x01fffff0u), 1, 1024 * 1024, f); fclose(f); LOG("[mp] rgb dump %s from %08x\n", path, last_dst); }
        }
        last_dst = ld32(RW(4) + 216) & 0x0fffffffu;
    }
    /* HP_MPDUMP=dir: save the raw8 YUV macroblocks of the first pictures handed to CSC */
    static int n;
    const char *dir = getenv("HP_MPDUMP");
    uint32_t p = RW(5), src = ld32(p) & 0x01fffff0u, mbs = ld32(p + 12) * ld32(p + 16);
    if (dir && mbs && n < 8) {
        char path[512]; snprintf(path, sizeof path, "%s/pic%d_%ux%u.yuv", dir, n++, ld32(p + 12), ld32(p + 16));
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(g_ram + src, 1, mbs * 384, f); fclose(f); LOG("[mp] dumped %s from %08x\n", path, src); }
        for (int b = 0; b < 3 && n == 2; b++) {   /* all three frame stores at the second conversion */
            snprintf(path, sizeof path, "%s/buf%d_32x32.yuv", dir, b);
            f = fopen(path, "wb");
            if (f) { fwrite(g_ram + 0x119b440 + b * 0x60000, 1, 0x60000, f); fclose(f); }
        }
    }
    f_002c9008_orig(c);
}

void hle_log_mprest(EE *c) {
    extern unsigned long long g_vsyncs;
    uint32_t s = RW(4);
    static int n;
    if (g_trace && n++ < 60) {
        uint32_t bp = ld32(s + 28);
        LOG("[mp v%llu] restore: madr %08x tadr %08x qwc %u chcr %08x | d3 %08x %u %08x | bp %u ifc %u fp %u\n", g_vsyncs,
            ld32(s), ld32(s + 4), ld32(s + 8), ld32(s + 12), ld32(s + 16), ld32(s + 20), ld32(s + 24), bp & 127, (bp >> 8) & 15, (bp >> 16) & 3);
    }
    f_002c9468_orig(c);
}

void hle_log_mpbits(EE *c) {
    static int n;
    uint32_t want = RW(5), ra = RW(31);
    f_002c7268_orig(c);
    if (getenv("HP_MPBITS") && n < 600) { n++; LOG("[mpbits] %2u bits -> %x (ra %08x)\n", want, RW(2), ra); }
}

void hle_log_mppic(EE *c) {
    static int n;
    const char *dir = getenv("HP_MPDUMP");
    uint32_t d = RW(4);
    if (dir && n < 4) {
        LOG("[mp] picture %d: refs 440=%08x 444=%08x 452=%08x\n", n, ld32(d + 440), ld32(d + 444), ld32(d + 452));
        for (int b = 0; b < 3; b++) {
            char path[512]; snprintf(path, sizeof path, "%s/p%d_buf%d_32x32.yuv", dir, n, b);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(g_ram + 0x119b440 + b * 0x60000, 1, 0x60000, f); fclose(f); }
        }
        n++;
    }
    f_002c75b0_orig(c);
}
