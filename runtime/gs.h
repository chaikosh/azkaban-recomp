/* Graphics Synthesizer interface shared by GIF/VIF/VU code and the display. */
#pragma once
#include <stdint.h>
#include "ee.h"

enum {
    PSMCT32 = 0x00, PSMCT24 = 0x01, PSMCT16 = 0x02, PSMCT16S = 0x0a, PSMT8 = 0x13, PSMT4 = 0x14,
    PSMT8H = 0x1b, PSMT4HL = 0x24, PSMT4HH = 0x2c, PSMZ32 = 0x30, PSMZ24 = 0x31, PSMZ16 = 0x32, PSMZ16S = 0x3a,
};

typedef struct {
    uint64_t prim, rgbaq, st, uv, fog, tex0[2], clamp[2], tex1[2], xyoffset[2], prmodecont, prmode, texclut,
        scanmsk, texa, fogcol, scissor[2], alpha[2], dimx, dthe, colclamp, test[2], pabe, fba[2], frame[2],
        zbuf[2], bitbltbuf, trxpos, trxreg, trxdir, label, miptbp1[2], miptbp2[2];
} GSState;

extern GSState gs;
extern uint8_t *g_vram;

void gs_init(void);
void gs_write_reg(int reg, uint64_t v);
void gif_feed(int path, const U128 *q, uint32_t n);
int gif_path_idle(int path);
uint32_t gs_read_raw(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x, uint32_t y);
void gs_write_raw(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x, uint32_t y, uint32_t v);
void gs_signal(uint64_t v);
void gs_finish(void);
uint64_t hw_gs_priv(uint32_t off);

/* display.c */
void display_vsync(void);          /* vblank: capture the displayed buffer */
void display_loop(void);           /* main thread: window, input, presentation */
