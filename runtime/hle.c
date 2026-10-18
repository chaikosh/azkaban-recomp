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
