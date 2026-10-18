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
void hle_log_queue(EE *c) {
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
