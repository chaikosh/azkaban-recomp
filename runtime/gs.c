/* Graphics Synthesizer: local memory, registers, GIF packets and a software
 * rasterizer that works directly on the emulated 4 MiB local memory, so
 * render-to-texture, CLUT loads and buffer copies behave like the hardware. */
#include "rt.h"
#include "gs.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

uint8_t *g_vram;
/* HP_GS=gpu: the GPU renderer (gs_bridge.cpp) draws; this file only keeps the register side */
int g_gs_gpu;
int gsb_enabled(void);
int gsb_init(void);
void gsb_gif(int path, const void *data, uint32_t qwc);
void gsb_read_image(uint8_t *dst, uint32_t qwords);
#define VRAM_WORDS (1u << 20)
#define V32(i) (((uint32_t *)g_vram)[(i) & (VRAM_WORDS - 1)])
#define V16(i) (((uint16_t *)g_vram)[(i) & (2 * VRAM_WORDS - 1)])
#define V8(i)  (g_vram[(i) & (4 * VRAM_WORDS - 1)])

/* ---- swizzle tables (GS local memory layout) -------------------------------- */
static const uint8_t blockTable32[4][8] = {
    {0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23},
    {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
static const uint8_t blockTable16[8][4] = {
    {0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15},
    {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
static const uint8_t blockTable16S[8][4] = {
    {0, 2, 16, 18}, {1, 3, 17, 19}, {8, 10, 24, 26}, {9, 11, 25, 27},
    {4, 6, 20, 22}, {5, 7, 21, 23}, {12, 14, 28, 30}, {13, 15, 29, 31}};
static const uint8_t blockTable8[4][8] = {
    {0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23},
    {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
static const uint8_t blockTable4[8][4] = {
    {0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15},
    {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
static const uint8_t columnTable32[8][8] = {
    {0, 1, 4, 5, 8, 9, 12, 13}, {2, 3, 6, 7, 10, 11, 14, 15},
    {16, 17, 20, 21, 24, 25, 28, 29}, {18, 19, 22, 23, 26, 27, 30, 31},
    {32, 33, 36, 37, 40, 41, 44, 45}, {34, 35, 38, 39, 42, 43, 46, 47},
    {48, 49, 52, 53, 56, 57, 60, 61}, {50, 51, 54, 55, 58, 59, 62, 63}};
static const uint8_t columnTable16[8][16] = {
    {0, 2, 8, 10, 16, 18, 24, 26, 1, 3, 9, 11, 17, 19, 25, 27},
    {4, 6, 12, 14, 20, 22, 28, 30, 5, 7, 13, 15, 21, 23, 29, 31},
    {32, 34, 40, 42, 48, 50, 56, 58, 33, 35, 41, 43, 49, 51, 57, 59},
    {36, 38, 44, 46, 52, 54, 60, 62, 37, 39, 45, 47, 53, 55, 61, 63},
    {64, 66, 72, 74, 80, 82, 88, 90, 65, 67, 73, 75, 81, 83, 89, 91},
    {68, 70, 76, 78, 84, 86, 92, 94, 69, 71, 77, 79, 85, 87, 93, 95},
    {96, 98, 104, 106, 112, 114, 120, 122, 97, 99, 105, 107, 113, 115, 121, 123},
    {100, 102, 108, 110, 116, 118, 124, 126, 101, 103, 109, 111, 117, 119, 125, 127}};
static const uint8_t columnTable8[16][16] = {
    {0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54},
    {8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62},
    {33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23},
    {41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31},
    {96, 100, 112, 116, 64, 68, 80, 84, 98, 102, 114, 118, 66, 70, 82, 86},
    {104, 108, 120, 124, 72, 76, 88, 92, 106, 110, 122, 126, 74, 78, 90, 94},
    {65, 69, 81, 85, 97, 101, 113, 117, 67, 71, 83, 87, 99, 103, 115, 119},
    {73, 77, 89, 93, 105, 109, 121, 125, 75, 79, 91, 95, 107, 111, 123, 127},
    {128, 132, 144, 148, 160, 164, 176, 180, 130, 134, 146, 150, 162, 166, 178, 182},
    {136, 140, 152, 156, 168, 172, 184, 188, 138, 142, 154, 158, 170, 174, 186, 190},
    {161, 165, 177, 181, 129, 133, 145, 149, 163, 167, 179, 183, 131, 135, 147, 151},
    {169, 173, 185, 189, 137, 141, 153, 157, 171, 175, 187, 191, 139, 143, 155, 159},
    {224, 228, 240, 244, 192, 196, 208, 212, 226, 230, 242, 246, 194, 198, 210, 214},
    {232, 236, 248, 252, 200, 204, 216, 220, 234, 238, 250, 254, 202, 206, 218, 222},
    {193, 197, 209, 213, 225, 229, 241, 245, 195, 199, 211, 215, 227, 231, 243, 247},
    {201, 205, 217, 221, 233, 237, 249, 253, 203, 207, 219, 223, 235, 239, 251, 255}};
static const uint16_t columnTable4[16][32] = {
    {0, 8, 32, 40, 64, 72, 96, 104, 2, 10, 34, 42, 66, 74, 98, 106, 4, 12, 36, 44, 68, 76, 100, 108, 6, 14, 38, 46, 70, 78, 102, 110},
    {16, 24, 48, 56, 80, 88, 112, 120, 18, 26, 50, 58, 82, 90, 114, 122, 20, 28, 52, 60, 84, 92, 116, 124, 22, 30, 54, 62, 86, 94, 118, 126},
    {65, 73, 97, 105, 1, 9, 33, 41, 67, 75, 99, 107, 3, 11, 35, 43, 69, 77, 101, 109, 5, 13, 37, 45, 71, 79, 103, 111, 7, 15, 39, 47},
    {81, 89, 113, 121, 17, 25, 49, 57, 83, 91, 115, 123, 19, 27, 51, 59, 85, 93, 117, 125, 21, 29, 53, 61, 87, 95, 119, 127, 23, 31, 55, 63},
    {192, 200, 224, 232, 128, 136, 160, 168, 194, 202, 226, 234, 130, 138, 162, 170, 196, 204, 228, 236, 132, 140, 164, 172, 198, 206, 230, 238, 134, 142, 166, 174},
    {208, 216, 240, 248, 144, 152, 176, 184, 210, 218, 242, 250, 146, 154, 178, 186, 212, 220, 244, 252, 148, 156, 180, 188, 214, 222, 246, 254, 150, 158, 182, 190},
    {129, 137, 161, 169, 193, 201, 225, 233, 131, 139, 163, 171, 195, 203, 227, 235, 133, 141, 165, 173, 197, 205, 229, 237, 135, 143, 167, 175, 199, 207, 231, 239},
    {145, 153, 177, 185, 209, 217, 241, 249, 147, 155, 179, 187, 211, 219, 243, 251, 149, 157, 181, 189, 213, 221, 245, 253, 151, 159, 183, 191, 215, 223, 247, 255},
    {256, 264, 288, 296, 320, 328, 352, 360, 258, 266, 290, 298, 322, 330, 354, 362, 260, 268, 292, 300, 324, 332, 356, 364, 262, 270, 294, 302, 326, 334, 358, 366},
    {272, 280, 304, 312, 336, 344, 368, 376, 274, 282, 306, 314, 338, 346, 370, 378, 276, 284, 308, 316, 340, 348, 372, 380, 278, 286, 310, 318, 342, 350, 374, 382},
    {321, 329, 353, 361, 257, 265, 289, 297, 323, 331, 355, 363, 259, 267, 291, 299, 325, 333, 357, 365, 261, 269, 293, 301, 327, 335, 359, 367, 263, 271, 295, 303},
    {337, 345, 369, 377, 273, 281, 305, 313, 339, 347, 371, 379, 275, 283, 307, 315, 341, 349, 373, 381, 277, 285, 309, 317, 343, 351, 375, 383, 279, 287, 311, 319},
    {448, 456, 480, 488, 384, 392, 416, 424, 450, 458, 482, 490, 386, 394, 418, 426, 452, 460, 484, 492, 388, 396, 420, 428, 454, 462, 486, 494, 390, 398, 422, 430},
    {464, 472, 496, 504, 400, 408, 432, 440, 466, 474, 498, 506, 402, 410, 434, 442, 468, 476, 500, 508, 404, 412, 436, 444, 470, 478, 502, 510, 406, 414, 438, 446},
    {385, 393, 417, 425, 449, 457, 481, 489, 387, 395, 419, 427, 451, 459, 483, 491, 389, 397, 421, 429, 453, 461, 485, 493, 391, 399, 423, 431, 455, 463, 487, 495},
    {401, 409, 433, 441, 465, 473, 497, 505, 403, 411, 435, 443, 467, 475, 499, 507, 405, 413, 437, 445, 469, 477, 501, 509, 407, 415, 439, 447, 471, 479, 503, 511}};

/* Element index of pixel (x, y) in a buffer with base block `bp` and width `bw`
 * (in 64-pixel units).  zx = 24 for the Z formats (block number XOR 24). */
static inline uint32_t a32(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t zx) {
    uint32_t page = (y >> 5) * bw + (x >> 6);
    uint32_t blk = bp + page * 32 + (blockTable32[(y >> 3) & 3][(x >> 3) & 7] ^ zx);
    return blk * 64 + columnTable32[y & 7][x & 7];
}
static inline uint32_t a16(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t zx) {
    uint32_t page = (y >> 6) * bw + (x >> 6);
    uint32_t blk = bp + page * 32 + (blockTable16[(y >> 3) & 7][(x >> 4) & 3] ^ zx);
    return blk * 128 + columnTable16[y & 7][x & 15];
}
static inline uint32_t a16s(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t zx) {
    uint32_t page = (y >> 6) * bw + (x >> 6);
    uint32_t blk = bp + page * 32 + (blockTable16S[(y >> 3) & 7][(x >> 4) & 3] ^ zx);
    return blk * 128 + columnTable16[y & 7][x & 15];
}
static inline uint32_t a8(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) {
    uint32_t pw = bw >> 1 ? bw >> 1 : 1;
    uint32_t page = (y >> 6) * pw + (x >> 7);
    uint32_t blk = bp + page * 32 + blockTable8[(y >> 4) & 3][(x >> 4) & 7];
    return blk * 256 + columnTable8[y & 15][x & 15];
}
static inline uint32_t a4(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) {
    uint32_t pw = bw >> 1 ? bw >> 1 : 1;
    uint32_t page = (y >> 7) * pw + (x >> 7);
    uint32_t blk = bp + page * 32 + blockTable4[(y >> 4) & 7][(x >> 5) & 3];
    return blk * 512 + columnTable4[y & 15][x & 31];
}

/* Raw read/write of a pixel in any storage format (no color conversion). */
uint32_t gs_read_raw(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x, uint32_t y) {
    switch (psm) {
    case PSMCT32: case PSMCT24: case PSMT8H: case PSMT4HL: case PSMT4HH: return V32(a32(bp, bw, x, y, 0));
    case PSMZ32: case PSMZ24: return V32(a32(bp, bw, x, y, 24));
    case PSMCT16: return V16(a16(bp, bw, x, y, 0));
    case PSMCT16S: return V16(a16s(bp, bw, x, y, 0));
    case PSMZ16: return V16(a16(bp, bw, x, y, 24));
    case PSMZ16S: return V16(a16s(bp, bw, x, y, 24));
    case PSMT8: return V8(a8(bp, bw, x, y));
    case PSMT4: { uint32_t n = a4(bp, bw, x, y); return (V8(n >> 1) >> ((n & 1) * 4)) & 15; }
    }
    return 0;
}

void gs_write_raw(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x, uint32_t y, uint32_t v) {
    switch (psm) {
    case PSMCT32: case PSMZ32: V32(a32(bp, bw, x, y, psm == PSMZ32 ? 24 : 0)) = v; return;
    case PSMCT24: case PSMZ24: { uint32_t i = a32(bp, bw, x, y, psm == PSMZ24 ? 24 : 0); V32(i) = (V32(i) & 0xff000000u) | (v & 0xffffff); return; }
    case PSMT8H: { uint32_t i = a32(bp, bw, x, y, 0); V32(i) = (V32(i) & 0x00ffffffu) | (v << 24); return; }
    case PSMT4HL: { uint32_t i = a32(bp, bw, x, y, 0); V32(i) = (V32(i) & 0xf0ffffffu) | ((v & 15) << 24); return; }
    case PSMT4HH: { uint32_t i = a32(bp, bw, x, y, 0); V32(i) = (V32(i) & 0x0fffffffu) | ((v & 15) << 28); return; }
    case PSMCT16: V16(a16(bp, bw, x, y, 0)) = (uint16_t)v; return;
    case PSMCT16S: V16(a16s(bp, bw, x, y, 0)) = (uint16_t)v; return;
    case PSMZ16: V16(a16(bp, bw, x, y, 24)) = (uint16_t)v; return;
    case PSMZ16S: V16(a16s(bp, bw, x, y, 24)) = (uint16_t)v; return;
    case PSMT8: V8(a8(bp, bw, x, y)) = (uint8_t)v; return;
    case PSMT4: {
        uint32_t n = a4(bp, bw, x, y), s = (n & 1) * 4;
        V8(n >> 1) = (uint8_t)((V8(n >> 1) & ~(15u << s)) | ((v & 15) << s));
        return;
    }
    }
}

/* Specialized accessors, chosen once per primitive instead of per pixel. */
typedef uint32_t (*RdFn)(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y);
typedef void (*WrFn)(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t v);
static uint32_t rd32(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) { return V32(a32(bp, bw, x, y, 0)); }
static uint32_t rdz32(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) { return V32(a32(bp, bw, x, y, 24)); }
static uint32_t rd16(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) { return V16(a16(bp, bw, x, y, 0)); }
static uint32_t rd16s(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) { return V16(a16s(bp, bw, x, y, 0)); }
static uint32_t rdz16(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) { return V16(a16(bp, bw, x, y, 24)); }
static uint32_t rdz16s(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) { return V16(a16s(bp, bw, x, y, 24)); }
static uint32_t rd8(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) { return V8(a8(bp, bw, x, y)); }
static uint32_t rd4(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) { uint32_t n = a4(bp, bw, x, y); return (V8(n >> 1) >> ((n & 1) * 4)) & 15; }
static void wr32(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t v) { V32(a32(bp, bw, x, y, 0)) = v; }
static void wrz32(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t v) { V32(a32(bp, bw, x, y, 24)) = v; }
static void wr24(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t v) { uint32_t i = a32(bp, bw, x, y, 0); V32(i) = (V32(i) & 0xff000000u) | (v & 0xffffff); }
static void wrz24(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t v) { uint32_t i = a32(bp, bw, x, y, 24); V32(i) = (V32(i) & 0xff000000u) | (v & 0xffffff); }
static void wr16(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t v) { V16(a16(bp, bw, x, y, 0)) = (uint16_t)v; }
static void wr16s(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t v) { V16(a16s(bp, bw, x, y, 0)) = (uint16_t)v; }
static void wrz16(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t v) { V16(a16(bp, bw, x, y, 24)) = (uint16_t)v; }
static void wrz16s(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t v) { V16(a16s(bp, bw, x, y, 24)) = (uint16_t)v; }
static void wrgen(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t v) { (void)bp; (void)bw; (void)x; (void)y; (void)v; }

static RdFn rd_for(uint32_t psm) {
    switch (psm) {
    case PSMCT32: case PSMCT24: case PSMT8H: case PSMT4HL: case PSMT4HH: return rd32;
    case PSMZ32: case PSMZ24: return rdz32;
    case PSMCT16: return rd16;
    case PSMCT16S: return rd16s;
    case PSMZ16: return rdz16;
    case PSMZ16S: return rdz16s;
    case PSMT8: return rd8;
    case PSMT4: return rd4;
    }
    return rd32;
}
static WrFn wr_for(uint32_t psm) {
    switch (psm) {
    case PSMCT32: return wr32;
    case PSMCT24: return wr24;
    case PSMZ32: return wrz32;
    case PSMZ24: return wrz24;
    case PSMCT16: return wr16;
    case PSMCT16S: return wr16s;
    case PSMZ16: return wrz16;
    case PSMZ16S: return wrz16s;
    }
    return wrgen;
}

/* ---- registers -------------------------------------------------------------- */
GSState gs;
static int cur_path;
unsigned g_xfer_count[3], g_prims_by_path[3];

static inline uint64_t bits(uint64_t v, int lo, int n) { return (v >> lo) & ((1ull << n) - 1); }

static uint32_t c16to32(uint32_t c, int ta0, int ta1, int aem) {
    uint32_t r = (c & 0x1f) << 3, g = ((c >> 5) & 0x1f) << 3, b = ((c >> 10) & 0x1f) << 3;
    uint32_t a = (c & 0x8000) ? (uint32_t)ta1 : (aem && !(c & 0x7fff) ? 0 : (uint32_t)ta0);
    return r | (g << 8) | (b << 16) | (a << 24);
}

/* ---- CLUT ------------------------------------------------------------------- */
static uint32_t clut[512];   /* raw entries: 32-bit colors or 16-bit values */
static uint32_t cbp0 = ~0u, cbp1 = ~0u;

static void clut_load(uint64_t tex0) {
    if (g_gs_gpu) return;
    uint32_t psm = bits(tex0, 20, 6), cbp = bits(tex0, 37, 14), cpsm = bits(tex0, 51, 4);
    uint32_t csm = bits(tex0, 55, 1), csa = bits(tex0, 56, 5), cld = bits(tex0, 61, 3);
    if (psm != PSMT8 && psm != PSMT8H && psm != PSMT4 && psm != PSMT4HL && psm != PSMT4HH) return;
    switch (cld) {
    case 0: return;
    case 1: break;
    case 2: cbp0 = cbp; break;
    case 3: cbp1 = cbp; break;
    case 4: if (cbp == cbp0) return; cbp0 = cbp; break;
    case 5: if (cbp == cbp1) return; cbp1 = cbp; break;
    default: return;
    }
    int is8 = psm == PSMT8 || psm == PSMT8H;
    uint32_t cpsm_s = cpsm == 0 ? PSMCT32 : (cpsm == 0x0a ? PSMCT16S : PSMCT16);
    if (csm) {   /* CSM2: one row of TEXCLUT */
        uint32_t cbw = bits(gs.texclut, 0, 6), cou = bits(gs.texclut, 6, 6) * 16, cov = bits(gs.texclut, 12, 10);
        int n = is8 ? 256 : 16;
        for (int i = 0; i < n; i++) clut[i] = gs_read_raw(cbp, cbw, cpsm_s, cou + i, cov);
        return;
    }
    if (is8) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t p = (i & ~0x18u) | ((i & 0x08) << 1) | ((i & 0x10) >> 1);
            clut[i] = gs_read_raw(cbp, 1, cpsm_s, p & 15, p >> 4);
        }
    } else {
        uint32_t off = (cpsm == 0 ? (csa & 15) : csa) * 16;
        for (uint32_t i = 0; i < 16; i++) clut[off + i] = gs_read_raw(cbp, 1, cpsm_s, i & 7, i >> 3);
    }
}

/* ---- textures --------------------------------------------------------------- */
typedef struct {
    uint32_t tbp, tbw, psm, tw, th, tcc, tfx, cpsm, csa;
    uint32_t wms, wmt, minu, maxu, minv, maxv;
    int ta0, ta1, aem;
    int linear;
    RdFn rd;
    /* mipmapping (TEX1, MIPTBP1/2): mip is set when MMIN selects a mipmap filter and MXL > 0 */
    int mip, mxl, mmin, lcm, lsh, minlin;
    float k;
    uint32_t mtbp[7], mtbw[7];
} TexState;

static inline uint32_t tex_texel(const TexState *t, int x, int y) {
    /* wrap */
    int w = 1 << t->tw, h = 1 << t->th;
    switch (t->wms) {
    case 0: x &= w - 1; break;
    case 1: x = x < 0 ? 0 : x >= w ? w - 1 : x; break;
    case 2: x = x < (int)t->minu ? (int)t->minu : x > (int)t->maxu ? (int)t->maxu : x; break;
    case 3: x = (x & (int)t->minu) | (int)t->maxu; break;
    }
    switch (t->wmt) {
    case 0: y &= h - 1; break;
    case 1: y = y < 0 ? 0 : y >= h ? h - 1 : y; break;
    case 2: y = y < (int)t->minv ? (int)t->minv : y > (int)t->maxv ? (int)t->maxv : y; break;
    case 3: y = (y & (int)t->minv) | (int)t->maxv; break;
    }
    uint32_t raw = t->rd(t->tbp, t->tbw, (uint32_t)x, (uint32_t)y);
    uint32_t idx;
    switch (t->psm) {
    case PSMCT32: return raw;
    case PSMCT24: {
        uint32_t rgb = raw & 0xffffff;
        uint32_t a = (t->aem && !rgb) ? 0 : (uint32_t)t->ta0;
        return rgb | (a << 24);
    }
    case PSMCT16: case PSMCT16S: return c16to32(raw, t->ta0, t->ta1, t->aem);
    case PSMT8: idx = raw; break;
    case PSMT8H: idx = raw >> 24; break;
    case PSMT4: idx = raw; break;
    case PSMT4HL: idx = (raw >> 24) & 15; break;
    case PSMT4HH: idx = raw >> 28; break;
    default: return 0;
    }
    if (t->psm == PSMT4 || t->psm == PSMT4HL || t->psm == PSMT4HH) idx += (t->cpsm == 0 ? (t->csa & 15) : t->csa) * 16;
    uint32_t c = clut[idx & 511];
    if (t->cpsm == 0) return c;
    return c16to32(c & 0xffff, t->ta0, t->ta1, t->aem);
}

static inline uint32_t lerp_rgba(uint32_t a, uint32_t b, int f /* 0..256 */) {
    uint32_t r = 0;
    for (int k = 0; k < 32; k += 8) {
        int ca = (a >> k) & 0xff, cb = (b >> k) & 0xff;
        r |= (uint32_t)((ca * (256 - f) + cb * f) >> 8) << k;
    }
    return r;
}

/* u, v in texels (texel centers at +0.5) */
static uint32_t tex_sample(const TexState *t, float u, float v) {
    if (!t->linear) return tex_texel(t, (int)floorf(u), (int)floorf(v));
    u -= 0.5f; v -= 0.5f;
    int x0 = (int)floorf(u), y0 = (int)floorf(v);
    int fu = (int)((u - x0) * 256), fv = (int)((v - y0) * 256);
    uint32_t c00 = tex_texel(t, x0, y0), c10 = tex_texel(t, x0 + 1, y0);
    uint32_t c01 = tex_texel(t, x0, y0 + 1), c11 = tex_texel(t, x0 + 1, y0 + 1);
    return lerp_rgba(lerp_rgba(c00, c10, fu), lerp_rgba(c01, c11, fu), fv);
}

/* Sample mip level `lv` (base pointers from MIPTBP1/2, size and coordinates halved per level). */
static uint32_t tex_sample_level(const TexState *t, int lv, float u, float v, int linear) {
    TexState l = *t;
    l.linear = linear;
    if (lv > 0) {
        l.tbp = t->mtbp[lv]; l.tbw = t->mtbw[lv];
        l.tw = t->tw > (uint32_t)lv ? t->tw - lv : 0; l.th = t->th > (uint32_t)lv ? t->th - lv : 0;
        l.minu >>= lv; l.maxu >>= lv; l.minv >>= lv; l.maxv >>= lv;
        float sc = 1.0f / (float)(1 << lv);
        u *= sc; v *= sc;
    }
    return tex_sample(&l, u, v);
}

/* Texture lookup with the GS level-of-detail rules: LOD = log2(1/|Q|) * 2^L + K (or K with LCM),
   LOD < 0 magnifies level 0 (MMAG), otherwise MMIN picks and filters the levels up to MXL.  Games
   stream only the levels a distant object needs and leave TBP0 pointing at nothing (0), so level 0
   must not be sampled when the LOD says otherwise. */
static uint32_t tex_sample_mip(const TexState *t, float u, float v, float q) {
    float lod = t->lcm ? t->k : log2f(1.0f / fmaxf(fabsf(q), 1e-30f)) * (float)(1 << t->lsh) + t->k;
    if (lod < 0.0f) return tex_sample_level(t, 0, u, v, t->linear);
    if (t->mmin == 2 || t->mmin == 4) {
        int lv = (int)(lod + 0.5f);
        return tex_sample_level(t, lv > t->mxl ? t->mxl : lv, u, v, t->minlin);
    }
    int l0 = (int)lod;
    if (l0 >= t->mxl) return tex_sample_level(t, t->mxl, u, v, t->minlin);
    int f = (int)((lod - (float)l0) * 256.0f);
    uint32_t a = tex_sample_level(t, l0, u, v, t->minlin);
    if (f <= 0) return a;
    return lerp_rgba(a, tex_sample_level(t, l0 + 1, u, v, t->minlin), f);
}

/* ---- drawing context ---------------------------------------------------------- */
typedef struct {
    /* frame / z */
    uint32_t fbp, fbw, fpsm, fbmsk, zbp, zpsm, zmsk;
    /* scissor */
    int scx0, scx1, scy0, scy1;
    /* test */
    int ate, atst, aref, afail, date, datm, zte, ztst;
    /* alpha */
    int ba, bb, bc, bd, fix, abe, pabe, fba, colclamp;
    int tme, iip, fge, fst;
    uint32_t fogcol;
    TexState tex;
    RdFn fbrd, zrd;
    WrFn fbwr, zwr;
} DrawCtx;

static void setup_ctx(DrawCtx *d) {
    uint64_t prim = (gs.prmodecont & 1) ? gs.prim : gs.prmode;
    int ctxt = bits(prim, 9, 1);
    d->iip = bits(prim, 3, 1); d->tme = bits(prim, 4, 1); d->fge = bits(prim, 5, 1);
    d->abe = bits(prim, 6, 1); d->fst = bits(prim, 8, 1);
    uint64_t fr = gs.frame[ctxt], zb = gs.zbuf[ctxt], te = gs.test[ctxt], al = gs.alpha[ctxt], sc = gs.scissor[ctxt];
    d->fbp = bits(fr, 0, 9) * 32; d->fbw = bits(fr, 16, 6); d->fpsm = bits(fr, 24, 6); d->fbmsk = (uint32_t)(fr >> 32);
    d->zbp = bits(zb, 0, 9) * 32; d->zpsm = bits(zb, 24, 4) | 0x30; d->zmsk = bits(zb, 32, 1);
    d->scx0 = bits(sc, 0, 11); d->scx1 = bits(sc, 16, 11); d->scy0 = bits(sc, 32, 11); d->scy1 = bits(sc, 48, 11);
    d->ate = bits(te, 0, 1); d->atst = bits(te, 1, 3); d->aref = bits(te, 4, 8); d->afail = bits(te, 12, 2);
    d->date = bits(te, 14, 1); d->datm = bits(te, 15, 1); d->zte = bits(te, 16, 1); d->ztst = bits(te, 17, 2);
    d->ba = bits(al, 0, 2); d->bb = bits(al, 2, 2); d->bc = bits(al, 4, 2); d->bd = bits(al, 6, 2); d->fix = bits(al, 32, 8);
    d->pabe = gs.pabe & 1; d->fba = gs.fba[ctxt] & 1; d->colclamp = gs.colclamp & 1;
    d->fogcol = (uint32_t)gs.fogcol & 0xffffff;
    TexState *t = &d->tex;
    uint64_t t0 = gs.tex0[ctxt], t1 = gs.tex1[ctxt], cl = gs.clamp[ctxt];
    t->tbp = bits(t0, 0, 14); t->tbw = bits(t0, 14, 6); t->psm = bits(t0, 20, 6);
    t->tw = bits(t0, 26, 4); t->th = bits(t0, 30, 4); t->tcc = bits(t0, 34, 1); t->tfx = bits(t0, 35, 2);
    t->cpsm = bits(t0, 51, 4); t->csa = bits(t0, 56, 5);
    if (t->tw > 10) t->tw = 10;
    if (t->th > 10) t->th = 10;
    t->wms = bits(cl, 0, 2); t->wmt = bits(cl, 2, 2);
    t->minu = bits(cl, 4, 10); t->maxu = bits(cl, 14, 10); t->minv = bits(cl, 24, 10); t->maxv = bits(cl, 34, 10);
    t->ta0 = bits(gs.texa, 0, 8); t->aem = bits(gs.texa, 15, 1); t->ta1 = bits(gs.texa, 32, 8);
    t->linear = bits(t1, 5, 1);   /* MMAG */
    t->rd = rd_for(t->psm);
    t->lcm = bits(t1, 0, 1); t->mxl = bits(t1, 2, 3); t->mmin = bits(t1, 6, 3); t->lsh = bits(t1, 19, 2);
    t->k = (float)((int32_t)((uint32_t)bits(t1, 32, 12) << 20) >> 20) / 16.0f;
    t->minlin = t->mmin == 1 || t->mmin == 4 || t->mmin == 5;
    t->mip = t->mmin >= 2 && t->mmin <= 5 && t->mxl > 0;
    if (t->mxl > 6) t->mxl = 6;
    if (t->mip) {
        t->mtbp[0] = t->tbp; t->mtbw[0] = t->tbw;
        uint64_t m1 = gs.miptbp1[ctxt], m2 = gs.miptbp2[ctxt];
        for (int k = 0; k < 3; k++) {
            t->mtbp[1 + k] = bits(m1, 20 * k, 14); t->mtbw[1 + k] = bits(m1, 20 * k + 14, 6);
            t->mtbp[4 + k] = bits(m2, 20 * k, 14); t->mtbw[4 + k] = bits(m2, 20 * k + 14, 6);
        }
        if (bits(t1, 9, 1)) {   /* MTBA: levels 1-3 follow level 0 in memory */
            static const int bpp[64] = {[PSMCT32] = 32, [PSMCT24] = 32, [PSMCT16] = 16, [PSMCT16S] = 16, [PSMT8] = 8, [PSMT4] = 4,
                                        [PSMT8H] = 32, [PSMT4HL] = 32, [PSMT4HH] = 32};
            uint32_t bp = t->tbp, bw = t->tbw, w = 1u << t->tw, h = 1u << t->th, b = bpp[t->psm & 63] ? bpp[t->psm & 63] : 32;
            for (int k = 1; k <= 3; k++) {
                bp += (w * h * b / 8) >> 8;
                w = w > 1 ? w >> 1 : 1; h = h > 1 ? h >> 1 : 1; bw = bw > 1 ? bw >> 1 : 1;
                t->mtbp[k] = bp; t->mtbw[k] = bw;
            }
        }
    }
    d->fbrd = rd_for(d->fpsm); d->fbwr = wr_for(d->fpsm);
    d->zrd = rd_for(d->zpsm); d->zwr = wr_for(d->zpsm);
}

/* ---- per-pixel pipeline --------------------------------------------------------- */
unsigned long long g_gs_nonzero;
static inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static inline uint32_t fb_read(const DrawCtx *d, int x, int y) {
    return d->fbrd(d->fbp, d->fbw, (uint32_t)x, (uint32_t)y);
}

static inline uint32_t fb_to_rgba(const DrawCtx *d, uint32_t v) {
    switch (d->fpsm) {
    case PSMCT32: case PSMZ32: return v;
    case PSMCT24: case PSMZ24: return (v & 0xffffff) | 0x80000000u;
    default: return c16to32(v, 0, 0x80, 0);
    }
}

/* Draw one pixel with interpolated color c (RGBA), depth z, fog f, and texture coords. */
static void put_pixel(const DrawCtx *d, int x, int y, uint32_t z, uint32_t col, int fog, float u, float v, float q) {
    if (x < d->scx0 || x > d->scx1 || y < d->scy0 || y > d->scy1) return;
    int r = col & 0xff, g = (col >> 8) & 0xff, b = (col >> 16) & 0xff, a = col >> 24;
    if (d->tme) {
        uint32_t t = d->tex.mip ? tex_sample_mip(&d->tex, u, v, q) : tex_sample(&d->tex, u, v);
        int tr = t & 0xff, tg = (t >> 8) & 0xff, tb = (t >> 16) & 0xff, ta = t >> 24;
        switch (d->tex.tfx) {
        case 0: r = (r * tr) >> 7; g = (g * tg) >> 7; b = (b * tb) >> 7; if (d->tex.tcc) a = (a * ta) >> 7; break;
        case 1: r = tr; g = tg; b = tb; if (d->tex.tcc) a = ta; break;
        case 2: r = ((r * tr) >> 7) + a; g = ((g * tg) >> 7) + a; b = ((b * tb) >> 7) + a; if (d->tex.tcc) a = a + ta; break;
        case 3: r = ((r * tr) >> 7) + a; g = ((g * tg) >> 7) + a; b = ((b * tb) >> 7) + a; if (d->tex.tcc) a = ta; break;
        }
        r = clamp255(r); g = clamp255(g); b = clamp255(b); a = clamp255(a);
    }
    if (d->fge) {
        int fr = d->fogcol & 0xff, fg = (d->fogcol >> 8) & 0xff, fb = (d->fogcol >> 16) & 0xff;
        r = (r * fog + fr * (255 - fog)) >> 8;
        g = (g * fog + fg * (255 - fog)) >> 8;
        b = (b * fog + fb * (255 - fog)) >> 8;
    }
    int write_fb = 1, write_z = !d->zmsk, write_alpha = 1;
    if (d->ate) {
        int pass;
        switch (d->atst) {
        case 0: pass = 0; break;
        case 1: pass = 1; break;
        case 2: pass = a < d->aref; break;
        case 3: pass = a <= d->aref; break;
        case 4: pass = a == d->aref; break;
        case 5: pass = a >= d->aref; break;
        case 6: pass = a > d->aref; break;
        default: pass = a != d->aref; break;
        }
        if (!pass) {
            switch (d->afail) {
            case 0: return;
            case 1: write_z = 0; break;
            case 2: write_fb = 0; break;
            case 3: write_z = 0; write_alpha = 0; break;
            }
        }
    }
    uint32_t dst = 0;
    int have_dst = 0;
    if (d->date) {
        dst = fb_read(d, x, y);
        have_dst = 1;
        int abit = (d->fpsm == PSMCT16 || d->fpsm == PSMCT16S) ? (dst >> 15) & 1 : (dst >> 31) & 1;
        if (abit != d->datm) return;
    }
    if (d->zte) {
        uint32_t zcur = d->zrd(d->zbp, d->fbw, (uint32_t)x, (uint32_t)y);
        switch (d->ztst) {
        case 0: return;
        case 1: break;
        case 2: if (z < zcur) return; break;
        case 3: if (z <= zcur) return; break;
        }
    } else write_z = 0;
    if (write_z) {
        uint32_t zv = z;
        if (d->zpsm == PSMZ24) zv = z > 0xffffff ? 0xffffff : z;
        else if (d->zpsm == PSMZ16 || d->zpsm == PSMZ16S) zv = z > 0xffff ? 0xffff : z;
        d->zwr(d->zbp, d->fbw, (uint32_t)x, (uint32_t)y, zv);
    }
    if (!write_fb) return;
    if (d->abe && (!d->pabe || (a & 0x80))) {
        if (!have_dst) dst = fb_read(d, x, y);
        uint32_t dc = fb_to_rgba(d, dst);
        int cs[3] = {r, g, b}, cd[3] = {(int)(dc & 0xff), (int)((dc >> 8) & 0xff), (int)((dc >> 16) & 0xff)};
        int ad = dc >> 24;
        int cc = d->bc == 0 ? a : d->bc == 1 ? ad : d->fix;
        for (int k = 0; k < 3; k++) {
            int A = d->ba == 0 ? cs[k] : d->ba == 1 ? cd[k] : 0;
            int B = d->bb == 0 ? cs[k] : d->bb == 1 ? cd[k] : 0;
            int D = d->bd == 0 ? cs[k] : d->bd == 1 ? cd[k] : 0;
            int v2 = (((A - B) * cc) >> 7) + D;
            cs[k] = d->colclamp ? clamp255(v2) : (v2 & 0xff);
        }
        r = cs[0]; g = cs[1]; b = cs[2];
    }
    if (d->fba) a |= 0x80;
    uint32_t out;
    switch (d->fpsm) {
    case PSMCT32: case PSMCT24: case PSMZ32: case PSMZ24:
        out = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
        break;
    default:
        out = (uint32_t)(r >> 3) | ((uint32_t)(g >> 3) << 5) | ((uint32_t)(b >> 3) << 10) | ((a & 0x80) ? 0x8000u : 0);
        break;
    }
    uint32_t msk = d->fbmsk;
    if (!write_alpha) msk |= (d->fpsm == PSMCT16 || d->fpsm == PSMCT16S) ? 0x80000000u : 0xff000000u;
    if (d->fpsm == PSMCT16 || d->fpsm == PSMCT16S) {
        /* FBMSK applies to the 16-bit packed value through the 32-bit mask bit positions */
        uint32_t m16 = ((msk >> 3) & 0x1f) | (((msk >> 11) & 0x1f) << 5) | (((msk >> 19) & 0x1f) << 10) | ((msk >> 31) << 15);
        if (m16) {
            if (!have_dst) dst = fb_read(d, x, y);
            out = (out & ~m16) | (dst & m16);
        }
    } else if (msk) {
        if (!have_dst) dst = fb_read(d, x, y);
        out = (out & ~msk) | (dst & msk);
    }
    d->fbwr(d->fbp, d->fbw, (uint32_t)x, (uint32_t)y, out);
}

/* ---- row-parallel worker pool ---------------------------------------------------- */
#include <pthread.h>
#include <unistd.h>
typedef void (*RowFn)(void *ctx, int y);
static struct { RowFn fn; void *ctx; int y1, total; volatile int next, done; } job;
static pthread_mutex_t pm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pcv = PTHREAD_COND_INITIALIZER, pdone = PTHREAD_COND_INITIALIZER;
static int job_gen, active, nworkers = -1;

static void run_rows(void) {
    for (;;) {
        int y = __atomic_fetch_add(&job.next, 1, __ATOMIC_ACQ_REL);
        if (y >= job.y1) break;
        job.fn(job.ctx, y);
        __atomic_fetch_add(&job.done, 1, __ATOMIC_ACQ_REL);
    }
}

static void *gs_worker(void *arg) {
    { extern void hp_thread_register(const char *); hp_thread_register("gs-worker"); }
    (void)arg;
    int seen = 0;
    for (;;) {
        pthread_mutex_lock(&pm);
        while (job_gen == seen) pthread_cond_wait(&pcv, &pm);
        seen = job_gen;
        active++;
        pthread_mutex_unlock(&pm);
        run_rows();
        pthread_mutex_lock(&pm);
        if (--active == 0) pthread_cond_signal(&pdone);
        pthread_mutex_unlock(&pm);
    }
    return NULL;
}

#include <time.h>
double g_prof[8];   /* 0 dma1, 1 sprite, 2 tri, 3 pool wait, 4 vu1, 5 gif */
static inline double pnow(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static void parallel_rows(int y0, int y1, int width, RowFn fn, void *ctx) {
    if (nworkers < 0) {
        const char *e = getenv("HP_GS_THREADS");
        nworkers = e ? atoi(e) : 3;
        for (int k = 0; k < nworkers; k++) { pthread_t t; pthread_create(&t, NULL, gs_worker, NULL); }
    }
    if (!nworkers || (y1 - y0) * width < 16384 || y1 - y0 < 8) {
        for (int y = y0; y < y1; y++) fn(ctx, y);
        return;
    }
    pthread_mutex_lock(&pm);
    job.fn = fn; job.ctx = ctx; job.y1 = y1; job.total = y1 - y0;
    job.next = y0; job.done = 0;
    job_gen++;
    pthread_cond_broadcast(&pcv);
    pthread_mutex_unlock(&pm);
    run_rows();
    double tw = pnow();
    pthread_mutex_lock(&pm);
    while (active > 0 || job.done < job.total) pthread_cond_wait(&pdone, &pm);
    pthread_mutex_unlock(&pm);
    g_prof[3] += pnow() - tw;
}

/* ---- primitives ------------------------------------------------------------------ */
typedef struct {
    float x, y;       /* window coordinates (pixels) */
    double z;
    float r, g, b, a, f;
    float s, t, q;    /* STQ */
    float u, v;       /* UV in texels */
} Vtx;

static Vtx vq[3];
static int vcount, vfan;

static Vtx make_vertex(uint64_t xyz, int has_f, uint8_t fval) {
    Vtx v;
    int ctxt = bits((gs.prmodecont & 1) ? gs.prim : gs.prmode, 9, 1);
    uint64_t off = gs.xyoffset[ctxt];
    int ofx = (int)bits(off, 0, 16), ofy = (int)bits(off, 32, 16);
    v.x = ((int)bits(xyz, 0, 16) - ofx) / 16.0f;
    v.y = ((int)bits(xyz, 16, 16) - ofy) / 16.0f;
    v.z = has_f ? (double)bits(xyz, 32, 24) : (double)(uint32_t)(xyz >> 32);
    v.f = has_f ? fval : (float)bits(gs.fog, 56, 8);
    v.r = (float)bits(gs.rgbaq, 0, 8); v.g = (float)bits(gs.rgbaq, 8, 8);
    v.b = (float)bits(gs.rgbaq, 16, 8); v.a = (float)bits(gs.rgbaq, 24, 8);
    union { uint32_t u; float f; } cv;
    cv.u = (uint32_t)(gs.rgbaq >> 32); v.q = cv.f;
    cv.u = (uint32_t)gs.st; v.s = cv.f;
    cv.u = (uint32_t)(gs.st >> 32); v.t = cv.f;
    v.u = (float)bits(gs.uv, 0, 14) / 16.0f;
    v.v = (float)bits(gs.uv, 16, 14) / 16.0f;
    return v;
}

static void tex_coords(const DrawCtx *d, const Vtx *v, float *u, float *w) {
    if (d->fst) { *u = v->u; *w = v->v; return; }
    float q = v->q != 0.0f ? v->q : 1.0f;
    *u = v->s / q * (float)(1 << d->tex.tw);
    *w = v->t / q * (float)(1 << d->tex.th);
}

static inline uint32_t pack_color(float r, float g, float b, float a) {
    return (uint32_t)clamp255((int)r) | ((uint32_t)clamp255((int)g) << 8) | ((uint32_t)clamp255((int)b) << 16) |
           ((uint32_t)clamp255((int)a) << 24);
}

unsigned long long g_gs_pixels, g_gs_prims;
static struct { uint32_t fbp, psm, tme; unsigned long long prims, nz; } fbstat[16];

void gs_fb_stats(void) {
    LOG("[stats] gs targets:");
    for (int k = 0; k < 16 && fbstat[k].prims; k++) {
        LOG(" fbp %x psm %x tme %u: %llu prims %llu nz;", fbstat[k].fbp, fbstat[k].psm, fbstat[k].tme, fbstat[k].prims, fbstat[k].nz);
        fbstat[k].prims = fbstat[k].nz = 0;
    }
    LOG("\n");
}


typedef struct {
    const DrawCtx *d;
    float x0, y0, dx, dy, ua, ub, va, vb, q;
    int ix0, ix1;
    uint32_t col, z, packed;
    int fog, simple;
} SpriteJob;

static void sprite_row(void *ctx, int y) {
    const SpriteJob *j = ctx;
    const DrawCtx *d = j->d;
    if (j->simple) {
        for (int x = j->ix0; x <= j->ix1; x++) d->fbwr(d->fbp, d->fbw, (uint32_t)x, (uint32_t)y, j->packed);
        return;
    }
    float ty = j->dy > 0 ? (y - j->y0) / j->dy : 0;
    float vv = j->va + (j->vb - j->va) * ty;
    float du = j->dx > 0 ? (j->ub - j->ua) / j->dx : 0;
    float uu = j->ua + du * (j->ix0 - j->x0);
    for (int x = j->ix0; x <= j->ix1; x++, uu += du) put_pixel(d, x, y, j->z, j->col, j->fog, uu, vv, j->q);
}

static void draw_sprite(const DrawCtx *d, const Vtx *v0, const Vtx *v1) {
    float x0 = fminf(v0->x, v1->x), x1 = fmaxf(v0->x, v1->x);
    float y0 = fminf(v0->y, v1->y), y1 = fmaxf(v0->y, v1->y);
    int ix0 = (int)ceilf(x0), ix1 = (int)ceilf(x1) - 1, iy0 = (int)ceilf(y0), iy1 = (int)ceilf(y1) - 1;
    if (ix0 < d->scx0) ix0 = d->scx0;
    if (iy0 < d->scy0) iy0 = d->scy0;
    if (ix1 > d->scx1) ix1 = d->scx1;
    if (iy1 > d->scy1) iy1 = d->scy1;
    if (ix1 < ix0 || iy1 < iy0) return;
    const Vtx *a = v0->x <= v1->x ? v0 : v1, *b = a == v0 ? v1 : v0;
    const Vtx *c = v0->y <= v1->y ? v0 : v1, *e = c == v0 ? v1 : v0;
    SpriteJob j;
    float ua, va, ub, vb, uc, vc2, ue, ve;
    tex_coords(d, a, &ua, &va); tex_coords(d, b, &ub, &vb);
    tex_coords(d, c, &uc, &vc2); tex_coords(d, e, &ue, &ve);
    j.d = d; j.x0 = x0; j.y0 = y0; j.dx = x1 - x0; j.dy = y1 - y0;
    j.ua = ua; j.ub = ub; j.va = vc2; j.vb = ve;
    j.ix0 = ix0; j.ix1 = ix1;
    j.col = pack_color(v1->r, v1->g, v1->b, v1->a);
    j.z = v1->z > 4294967295.0 ? 0xffffffffu : (uint32_t)v1->z;
    j.fog = (int)v1->f;
    j.q = d->fst ? 1.0f : v1->q;
    /* plain fills: no texture, test, blend, fog, mask or depth work */
    j.simple = !d->tme && !d->abe && !d->ate && !d->date && !d->fge && !d->fbmsk && !d->fba &&
               (!d->zte || (d->ztst == 1 && d->zmsk)) &&
               (d->fpsm == PSMCT32 || d->fpsm == PSMCT24 || d->fpsm == PSMCT16 || d->fpsm == PSMCT16S);
    if (j.simple) {
        uint32_t cc = j.col;
        if (d->fpsm == PSMCT16 || d->fpsm == PSMCT16S)
            j.packed = ((cc & 0xff) >> 3) | (((cc >> 8) & 0xff) >> 3 << 5) | (((cc >> 16) & 0xff) >> 3 << 10) | ((cc >> 31) << 15);
        else j.packed = cc;
    }
    parallel_rows(iy0, iy1 + 1, ix1 - ix0 + 1, sprite_row, &j);
    g_gs_pixels += (unsigned long long)(ix1 - ix0 + 1) * (iy1 - iy0 + 1);
}

typedef struct {
    const DrawCtx *d;
    const Vtx *v0, *v1, *v2;
    float s0, t0, q0, s1, t1, q1, s2, t2, q2, tws, ths, inv;
    int ix0, ix1;
} TriJob;

static void tri_row(void *ctx, int y) {
    const TriJob *j = ctx;
    const DrawCtx *d = j->d;
    const Vtx *v0 = j->v0, *v1 = j->v1, *v2 = j->v2;
    float py = (float)y;
    /* edge functions are linear in x: evaluate at ix0 and step */
    float px = (float)j->ix0;
    float w0 = ((v1->x - px) * (v2->y - py) - (v2->x - px) * (v1->y - py)) * j->inv;
    float w1 = ((v2->x - px) * (v0->y - py) - (v0->x - px) * (v2->y - py)) * j->inv;
    float dw0 = -(v2->y - py - (v1->y - py)) * j->inv;   /* d/dx of w0 */
    float dw1 = -(v0->y - py - (v2->y - py)) * j->inv;
    dw0 = (v1->y - v2->y) * j->inv;
    dw1 = (v2->y - v0->y) * j->inv;
    uint32_t flatcol = pack_color(v2->r, v2->g, v2->b, v2->a);
    for (int x = j->ix0; x <= j->ix1; x++, w0 += dw0, w1 += dw1) {
        float w2 = 1.0f - w0 - w1;
        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
        double z = v0->z * w0 + v1->z * w1 + v2->z * w2;
        uint32_t zi = z <= 0 ? 0 : z >= 4294967295.0 ? 0xffffffffu : (uint32_t)z;
        uint32_t col;
        int fog;
        if (d->iip) {
            col = pack_color(v0->r * w0 + v1->r * w1 + v2->r * w2, v0->g * w0 + v1->g * w1 + v2->g * w2,
                             v0->b * w0 + v1->b * w1 + v2->b * w2, v0->a * w0 + v1->a * w1 + v2->a * w2);
            fog = (int)(v0->f * w0 + v1->f * w1 + v2->f * w2);
        } else {
            col = flatcol;
            fog = (int)v2->f;
        }
        float uu = 0, vv = 0, qq = 1.0f;
        if (d->tme) {
            float s = j->s0 * w0 + j->s1 * w1 + j->s2 * w2, t = j->t0 * w0 + j->t1 * w1 + j->t2 * w2;
            if (d->fst) { uu = s; vv = t; }
            else {
                float q = j->q0 * w0 + j->q1 * w1 + j->q2 * w2;
                if (q == 0) q = 1e-6f;
                uu = s / q * j->tws; vv = t / q * j->ths; qq = q;
            }
        }
        put_pixel(d, x, y, zi, col, fog, uu, vv, qq);
    }
}

static void draw_triangle(const DrawCtx *d, const Vtx *v0, const Vtx *v1, const Vtx *v2) {
    float area = (v1->x - v0->x) * (v2->y - v0->y) - (v2->x - v0->x) * (v1->y - v0->y);
    if (area == 0.0f) return;
    float minx = fminf(v0->x, fminf(v1->x, v2->x)), maxx = fmaxf(v0->x, fmaxf(v1->x, v2->x));
    float miny = fminf(v0->y, fminf(v1->y, v2->y)), maxy = fmaxf(v0->y, fmaxf(v1->y, v2->y));
    int ix0 = (int)ceilf(minx), ix1 = (int)ceilf(maxx) - 1, iy0 = (int)ceilf(miny), iy1 = (int)ceilf(maxy) - 1;
    if (ix0 < d->scx0) ix0 = d->scx0;
    if (iy0 < d->scy0) iy0 = d->scy0;
    if (ix1 > d->scx1) ix1 = d->scx1;
    if (iy1 > d->scy1) iy1 = d->scy1;
    if (ix0 > ix1 || iy0 > iy1) return;
    TriJob j;
    j.d = d; j.v0 = v0; j.v1 = v1; j.v2 = v2;
    j.q0 = j.q1 = j.q2 = 1;
    if (d->fst) { j.s0 = v0->u; j.t0 = v0->v; j.s1 = v1->u; j.t1 = v1->v; j.s2 = v2->u; j.t2 = v2->v; }
    else {
        j.s0 = v0->s; j.t0 = v0->t; j.q0 = v0->q; j.s1 = v1->s; j.t1 = v1->t; j.q1 = v1->q;
        j.s2 = v2->s; j.t2 = v2->t; j.q2 = v2->q;
    }
    j.tws = (float)(1 << d->tex.tw); j.ths = (float)(1 << d->tex.th);
    j.inv = 1.0f / area;
    j.ix0 = ix0; j.ix1 = ix1;
    parallel_rows(iy0, iy1 + 1, ix1 - ix0 + 1, tri_row, &j);
    g_gs_pixels += (unsigned long long)(ix1 - ix0 + 1) * (iy1 - iy0 + 1) / 2;
}

static void draw_line(const DrawCtx *d, const Vtx *v0, const Vtx *v1) {
    float dx = v1->x - v0->x, dy = v1->y - v0->y;
    int n = (int)ceilf(fmaxf(fabsf(dx), fabsf(dy)));
    if (n <= 0) n = 1;
    for (int k = 0; k < n; k++) {
        float t = (float)k / n;
        int x = (int)floorf(v0->x + dx * t), y = (int)floorf(v0->y + dy * t);
        const Vtx *c = d->iip ? NULL : v1;
        uint32_t col = c ? pack_color(c->r, c->g, c->b, c->a)
                         : pack_color(v0->r + (v1->r - v0->r) * t, v0->g + (v1->g - v0->g) * t,
                                      v0->b + (v1->b - v0->b) * t, v0->a + (v1->a - v0->a) * t);
        double z = v0->z + (v1->z - v0->z) * t;
        float u0, w0, u1, w1;
        tex_coords(d, v0, &u0, &w0); tex_coords(d, v1, &u1, &w1);
        put_pixel(d, x, y, (uint32_t)z, col, (int)(v0->f + (v1->f - v0->f) * t), u0 + (u1 - u0) * t, w0 + (w1 - w0) * t,
                  d->fst ? 1.0f : v0->q + (v1->q - v0->q) * t);
    }
}

/* debug: last host->local upload per destination base pointer (for HP_TEXDUMP) */
static struct { uint32_t pkt, data, psm, w, h, bw; unsigned long long vs; } upl[16384];
static int upl_first;
/* HP_TEXDUMP=dir [HP_TEXDUMPAT=vsync]: write every distinct texture (TEX0 + CLUT contents) the
   primitives sample from that vsync on as dir/tex_<n>.ppm, with a log line naming its last upload. */
static void tex_dump_check(void) {
    static const char *dir = (const char *)1;
    static long long at; static int count;
    static uint64_t seen[1024];
    extern unsigned long long g_vsyncs;
    if (dir == (const char *)1) { dir = getenv("HP_TEXDUMP"); at = getenv("HP_TEXDUMPAT") ? atoll(getenv("HP_TEXDUMPAT")) : 0; }
    if (!dir || (long long)g_vsyncs < at || count >= 1024) return;
    DrawCtx d; setup_ctx(&d);
    if (!d.tme) return;
    uint64_t t0 = gs.tex0[bits((gs.prmodecont & 1) ? gs.prim : gs.prmode, 9, 1)];
    uint64_t key = t0 & ~(7ull << 61) & ~(3ull << 35) & ~(1ull << 34);
    int is_idx = d.tex.psm == PSMT8 || d.tex.psm == PSMT8H || d.tex.psm == PSMT4 || d.tex.psm == PSMT4HL || d.tex.psm == PSMT4HH;
    if (is_idx) { uint64_t h = 1469598103934665603ull; for (int i = 0; i < 512; i++) { h ^= clut[i]; h *= 1099511628211ull; } key ^= h; }
    for (int i = 0; i < count; i++) if (seen[i] == key) return;
    seen[count] = key;
    TexState t = d.tex; t.wms = t.wmt = 0;
    int w = 1 << t.tw, h = 1 << t.th;
    if (w > 512) w = 512;
    if (h > 512) h = 512;
    char path[1200];
    snprintf(path, sizeof path, "%s/tex_%04d.ppm", dir, count);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { uint32_t c = tex_texel(&t, x, y); uint8_t px[3] = {(uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16)}; fwrite(px, 1, 3, f); }
        fclose(f);
    }
    uint32_t cbp = bits(t0, 37, 14);
    uint32_t tb = t.tbp & 16383, cb = cbp & 16383;
    LOG("[texdump v%llu] tex_%04d tex0 %016llx tbp %x tbw %u psm %x %dx%d cbp %x cpsm %x csa %u | tex upload v%llu pkt %08x data %08x psm %x %ux%u bw %u | clut upload v%llu pkt %08x data %08x psm %x %ux%u\n",
        g_vsyncs, count, (unsigned long long)t0, t.tbp, t.tbw, t.psm, 1 << t.tw, 1 << t.th, cbp, t.cpsm, t.csa,
        upl[tb].vs, upl[tb].pkt, upl[tb].data, upl[tb].psm, upl[tb].w, upl[tb].h, upl[tb].bw,
        upl[cb].vs, upl[cb].pkt, upl[cb].data, upl[cb].psm, upl[cb].w, upl[cb].h);
    count++;
}

static int primlog = -1;
static void vertex_kick(Vtx v, int draw) {
    if (g_gs_gpu) { g_gs_prims += draw; return; }
    uint64_t prim = gs.prim;
    int type = bits(prim, 0, 3);
    static long primskip = -1;
    if (primlog < 0) { primlog = getenv("HP_PRIMLOG") ? atoi(getenv("HP_PRIMLOG")) : 0; primskip = getenv("HP_PRIMSKIP") ? atol(getenv("HP_PRIMSKIP")) : 0; }
    static long primafter = -1;
    extern unsigned long long g_vsyncs;
    if (primafter < 0) primafter = getenv("HP_PRIMAFTER") ? atol(getenv("HP_PRIMAFTER")) : 0;
    if (primskip > 0) primskip--;
    else if (primlog > 0 && (long long)g_vsyncs >= primafter) {
        primlog--;
        LOG("[prim] %llx %s v(%.1f,%.1f z=%.0f rgba=%.0f,%.0f,%.0f,%.0f uv=%.1f,%.1f stq=%g,%g,%g) frame %llx test %llx alpha %llx tex0 %llx tex1 %llx clamp %llx\n",
            (unsigned long long)prim, draw ? "kick" : "noki", v.x, v.y, v.z, v.r, v.g, v.b, v.a, v.u, v.v, v.s, v.t, v.q,
            (unsigned long long)gs.frame[bits(prim, 9, 1)], (unsigned long long)gs.test[bits(prim, 9, 1)],
            (unsigned long long)gs.alpha[bits(prim, 9, 1)], (unsigned long long)gs.tex0[bits(prim, 9, 1)],
            (unsigned long long)gs.tex1[bits(prim, 9, 1)], (unsigned long long)gs.clamp[bits(prim, 9, 1)]);
    }
    vq[vcount++] = v;
    if (!draw) {
        /* XYZ3: vertex is queued but no primitive is drawn */
        if (type == 4 && vcount == 3) { vq[0] = vq[1]; vq[1] = vq[2]; vcount = 2; }
        else if (type == 5 && vcount == 3) { vq[1] = vq[2]; vcount = 2; }
        else if (vcount == 3) vcount = 0;
        return;
    }
    DrawCtx d;
    switch (type) {
    case 0:
        setup_ctx(&d);
        put_pixel(&d, (int)vq[0].x, (int)vq[0].y, (uint32_t)vq[0].z, pack_color(vq[0].r, vq[0].g, vq[0].b, vq[0].a),
                  (int)vq[0].f, vq[0].u, vq[0].v, d.fst ? 1.0f : vq[0].q);
        vcount = 0;
        break;
    case 1:
        if (vcount < 2) return;
        setup_ctx(&d); draw_line(&d, &vq[0], &vq[1]); vcount = 0; break;
    case 2:
        if (vcount < 2) return;
        setup_ctx(&d); draw_line(&d, &vq[0], &vq[1]); vq[0] = vq[1]; vcount = 1; break;
    case 3:
        if (vcount < 3) return;
        { double t0 = pnow(); setup_ctx(&d); draw_triangle(&d, &vq[0], &vq[1], &vq[2]); g_prof[2] += pnow() - t0; }
        vcount = 0; break;
    case 4:
        if (vcount < 3) return;
        { double t0 = pnow(); setup_ctx(&d); draw_triangle(&d, &vq[0], &vq[1], &vq[2]); g_prof[2] += pnow() - t0; }
        vq[0] = vq[1]; vq[1] = vq[2]; vcount = 2; break;
    case 5:
        if (vcount < 3) return;
        setup_ctx(&d); draw_triangle(&d, &vq[0], &vq[1], &vq[2]);
        vq[1] = vq[2]; vcount = 2; break;
    case 6:
        if (vcount < 2) return;
        { double t0 = pnow(); setup_ctx(&d); draw_sprite(&d, &vq[0], &vq[1]); g_prof[1] += pnow() - t0; }
        vcount = 0; break;
    default:
        vcount = 0; break;
    }
    g_gs_prims++;
    if (cur_path >= 0 && cur_path < 3) g_prims_by_path[cur_path]++;
    tex_dump_check();
    if (type <= 6) {
        DrawCtx d2; setup_ctx(&d2);
        int k;
        for (k = 0; k < 16; k++) if (!fbstat[k].prims || (fbstat[k].fbp == d2.fbp && fbstat[k].psm == d2.fpsm && fbstat[k].tme == (uint32_t)d2.tme)) break;
        if (k < 16) {
            if (!fbstat[k].prims) { fbstat[k].fbp = d2.fbp; fbstat[k].psm = d2.fpsm; fbstat[k].tme = d2.tme; }
            fbstat[k].prims++;
            fbstat[k].nz = g_gs_nonzero;
        }
    }
}

/* ---- transfers ------------------------------------------------------------------- */
static struct {
    int active, dir;
    uint32_t dbp, dbw, dpsm, x, y, sx, w, h;
    uint64_t pending; int pend_bits;
} xfer;
static struct { int active; uint32_t sbp, sbw, spsm, sx, sy, w, h, x, y; uint64_t acc; int acc_bits; } dl;

static int xfer_logged;
static void xfer_put_pixel(uint32_t v) {
    if (xfer.y >= xfer.h) return;
    gs_write_raw(xfer.dbp, xfer.dbw, xfer.dpsm, xfer.sx + xfer.x, (uint32_t)(bits(gs.trxpos, 48, 11)) + xfer.y, v);
    if (++xfer.x >= xfer.w) { xfer.x = 0; xfer.y++; }
}

static void xfer_start(void) {
    if (g_gs_gpu) { xfer.active = 0; dl.active = 0; return; }
    int dir = bits(gs.trxdir, 0, 2);
    uint64_t bb = gs.bitbltbuf;
    uint32_t w = bits(gs.trxreg, 0, 12), h = bits(gs.trxreg, 32, 12);
    static int xl = -1;
    static long long xafter;
    extern unsigned long long g_vsyncs;
    if (xl < 0) { xl = getenv("HP_XFERLOG") ? atoi(getenv("HP_XFERLOG")) : 0; xafter = getenv("HP_XFERAFTER") ? atoll(getenv("HP_XFERAFTER")) : 0; }
    xfer_logged = 0;
    if (xl > 0 && (long long)g_vsyncs >= xafter) {
        xl--; xfer_logged = 1;
        extern uint32_t g_dma_src;
        LOG("[gs xfer v%llu] src ram %08x path %d dir %d src bp %llx bw %llu psm %llx -> dst bp %llx bw %llu psm %llx, %ux%u at %llu,%llu\n", g_vsyncs, g_dma_src, cur_path, dir,
            (unsigned long long)bits(bb, 0, 14), (unsigned long long)bits(bb, 16, 6), (unsigned long long)bits(bb, 24, 6),
            (unsigned long long)bits(bb, 32, 14), (unsigned long long)bits(bb, 48, 6), (unsigned long long)bits(bb, 56, 6), w, h,
            (unsigned long long)bits(gs.trxpos, 32, 11), (unsigned long long)bits(gs.trxpos, 48, 11));
    }
    if (cur_path >= 0) g_xfer_count[cur_path]++;
    if (dir == 0) {
        xfer.active = 1; xfer.dir = 0;
        xfer.dbp = bits(bb, 32, 14); xfer.dbw = bits(bb, 48, 6); xfer.dpsm = bits(bb, 56, 6);
        xfer.sx = bits(gs.trxpos, 32, 11); xfer.x = 0; xfer.y = 0; xfer.w = w; xfer.h = h;
        xfer.pending = 0; xfer.pend_bits = 0;
        { extern uint32_t g_dma_src; uint32_t k = xfer.dbp & 16383;
          upl[k].pkt = g_dma_src; upl[k].data = 0; upl[k].psm = xfer.dpsm; upl[k].w = w; upl[k].h = h; upl[k].bw = xfer.dbw; upl[k].vs = g_vsyncs; upl_first = 1; }
    } else if (dir == 2) {
        uint32_t sbp = bits(bb, 0, 14), sbw = bits(bb, 16, 6), spsm = bits(bb, 24, 6);
        uint32_t dbp = bits(bb, 32, 14), dbw = bits(bb, 48, 6), dpsm = bits(bb, 56, 6);
        uint32_t ssx = bits(gs.trxpos, 0, 11), ssy = bits(gs.trxpos, 16, 11);
        uint32_t dsx = bits(gs.trxpos, 32, 11), dsy = bits(gs.trxpos, 48, 11);
        int order = bits(gs.trxpos, 59, 2);
        for (uint32_t yy = 0; yy < h; yy++) {
            uint32_t y = (order & 2) ? h - 1 - yy : yy;
            for (uint32_t xx = 0; xx < w; xx++) {
                uint32_t x = (order & 1) ? w - 1 - xx : xx;
                gs_write_raw(dbp, dbw, dpsm, dsx + x, dsy + y, gs_read_raw(sbp, sbw, spsm, ssx + x, ssy + y));
            }
        }
        xfer.active = 0;
    } else if (dir == 1) {   /* local->host: the EE reads the rectangle through the VIF1 FIFO */
        dl.active = 1;
        dl.sbp = bits(bb, 0, 14); dl.sbw = bits(bb, 16, 6); dl.spsm = bits(bb, 24, 6);
        dl.sx = bits(gs.trxpos, 0, 11); dl.sy = bits(gs.trxpos, 16, 11);
        dl.w = w; dl.h = h; dl.x = 0; dl.y = 0;
        dl.acc = 0; dl.acc_bits = 0;
        xfer.active = 0;
    } else {
        xfer.active = 0;
    }
}

/* Next pixel of a local->host transfer, or -1 when the rectangle is done. */
static int64_t dl_next_pixel(void) {
    if (!dl.active || dl.y >= dl.h) { dl.active = 0; return -1; }
    uint32_t v = gs_read_raw(dl.sbp, dl.sbw, dl.spsm, dl.sx + dl.x, dl.sy + dl.y);
    if (++dl.x >= dl.w) { dl.x = 0; dl.y++; }
    return v;
}

/* Fill `qwords` quadwords with downloaded image data in the source format (zeros past the end). */
void gs_read_image(uint8_t *dst, uint32_t qwords) {
    if (g_gs_gpu) { gsb_read_image(dst, qwords); return; }
    unsigned bpp;
    switch (dl.spsm) {
    case PSMCT32: case PSMZ32: bpp = 32; break;
    case PSMCT24: case PSMZ24: bpp = 24; break;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: bpp = 16; break;
    case PSMT8: case PSMT8H: bpp = 8; break;
    default: bpp = 4; break;
    }
    for (uint32_t q = 0; q < qwords; q++) {
        uint8_t out[16];
        for (int b = 0; b < 16; b++) {
            while (dl.acc_bits < 8) {
                int64_t px = dl_next_pixel();
                if (px < 0) { dl.acc |= 0; dl.acc_bits = 8; break; }
                dl.acc |= ((uint64_t)px & ((1ull << bpp) - 1)) << dl.acc_bits;
                dl.acc_bits += bpp;
            }
            out[b] = (uint8_t)dl.acc;
            dl.acc >>= 8;
            dl.acc_bits -= 8;
        }
        memcpy(dst + 16 * q, out, 16);
    }
}

/* IMAGE data for a host->local transfer, 64 bits at a time */
static unsigned long long xfer_words, xfer_nonzero;
static void xfer_data(uint64_t v) {
    if (!xfer.active) return;
    if (upl_first) { extern uint32_t g_dma_src; upl[xfer.dbp & 16383].data = g_dma_src; upl_first = 0; }
    xfer_words++; if (v) xfer_nonzero++;
    switch (xfer.dpsm) {
    case PSMCT32: case PSMZ32:
        xfer_put_pixel((uint32_t)v); xfer_put_pixel((uint32_t)(v >> 32)); break;
    case PSMCT24: case PSMZ24: {
        /* packed 3-byte pixels across 64-bit words */
        for (int k = 0; k < 8; k++) {
            xfer.pending |= ((v >> (8 * k)) & 0xff) << xfer.pend_bits;
            xfer.pend_bits += 8;
            if (xfer.pend_bits == 24) { xfer_put_pixel((uint32_t)xfer.pending); xfer.pending = 0; xfer.pend_bits = 0; }
        }
        break;
    }
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S:
        for (int k = 0; k < 4; k++) xfer_put_pixel((uint32_t)(v >> (16 * k)) & 0xffff);
        break;
    case PSMT8: case PSMT8H:
        for (int k = 0; k < 8; k++) xfer_put_pixel((uint32_t)(v >> (8 * k)) & 0xff);
        break;
    case PSMT4: case PSMT4HL: case PSMT4HH:
        for (int k = 0; k < 16; k++) xfer_put_pixel((uint32_t)(v >> (4 * k)) & 0xf);
        break;
    }
    if (xfer.y >= xfer.h) {
        xfer.active = 0;
        if (xfer_logged) { extern unsigned long long g_vsyncs; LOG("[gs xfer v%llu] done dst bp %x: %llu words, %llu nonzero\n", g_vsyncs, xfer.dbp, xfer_words, xfer_nonzero); }
        xfer_words = xfer_nonzero = 0;
    }
}

/* ---- register writes ----------------------------------------------------------------- */
uint32_t g_last_xgkick = 0, g_last_vu1_start = 0;
static int frame_plausible(uint64_t v) {
    uint32_t fbw = bits(v, 16, 6), psm = bits(v, 24, 6);
    int okpsm = psm == 0 || psm == 1 || psm == 2 || psm == 0xa || psm == 0x30 || psm == 0x31 || psm == 0x32 || psm == 0x3a;
    return okpsm && fbw <= 32 && bits(v, 30, 2) == 0;
}
void gs_write_reg(int reg, uint64_t v) {
    if ((reg == 0x4c || reg == 0x4d) && !frame_plausible(v)) {   /* HP_GSCHECK: report implausible FRAME writes */
        static int n = -1; if (n < 0) n = getenv("HP_GSCHECK") ? 12 : 0;
        if (n > 0) { n--; extern unsigned long long g_vsyncs;
            LOG("[gscheck v%llu] FRAME_%d = %016llx via path %d (last xgkick %03x, VU1 start %04x)\n", g_vsyncs, reg - 0x4b, (unsigned long long)v, cur_path, g_last_xgkick, g_last_vu1_start);
            if (cur_path == 0) { extern void vu1_snapshot_save(void); vu1_snapshot_save(); } }
    }
    static int ctx2log = -1;
    if (ctx2log < 0) ctx2log = getenv("HP_CTX2LOG") ? 200 : 0;
    if (ctx2log > 0 && (reg == 0x07 || reg == 0x09 || reg == 0x15 || reg == 0x19 || reg == 0x41 || reg == 0x43 ||
                        reg == 0x48 || reg == 0x4b || reg == 0x4d || reg == 0x4f)) {
        ctx2log--;
        LOG("[gs] ctx2 reg %02x = %016llx\n", reg, (unsigned long long)v);
    }
    switch (reg) {
    case 0x00: gs.prim = v; vcount = 0; break;
    case 0x01: gs.rgbaq = v; break;
    case 0x02: gs.st = v; break;
    case 0x03: gs.uv = v; break;
    case 0x04: gs.fog = (v >> 56) << 56; vertex_kick(make_vertex(v, 1, (uint8_t)(v >> 56)), 1); break;
    case 0x05: vertex_kick(make_vertex(v, 0, 0), 1); break;
    case 0x06: case 0x07: gs.tex0[reg - 6] = v; clut_load(v); break;
    case 0x08: case 0x09: gs.clamp[reg - 8] = v; break;
    case 0x0a: gs.fog = v; break;
    case 0x0c: gs.fog = (v >> 56) << 56; vertex_kick(make_vertex(v, 1, (uint8_t)(v >> 56)), 0); break;
    case 0x0d: vertex_kick(make_vertex(v, 0, 0), 0); break;
    case 0x14: case 0x15: gs.tex1[reg - 0x14] = v; break;
    case 0x16: case 0x17: {   /* TEX2: only the CLUT/format fields of TEX0 */
        uint64_t mask = (0x3full << 20) | (0xffffffffull << 37);
        int k = reg - 0x16;
        gs.tex0[k] = (gs.tex0[k] & ~mask) | (v & mask);
        clut_load(gs.tex0[k]);
        break;
    }
    case 0x18: case 0x19: gs.xyoffset[reg - 0x18] = v; break;
    case 0x1a: gs.prmodecont = v; break;
    case 0x1b: gs.prmode = v; break;
    case 0x1c: gs.texclut = v; break;
    case 0x22: gs.scanmsk = v; break;
    case 0x34: case 0x35: gs.miptbp1[reg - 0x34] = v; break;
    case 0x36: case 0x37: gs.miptbp2[reg - 0x36] = v; break;
    case 0x3b: gs.texa = v; break;
    case 0x3d: gs.fogcol = v; break;
    case 0x3f: break;   /* TEXFLUSH */
    case 0x40: case 0x41: gs.scissor[reg - 0x40] = v; break;
    case 0x42: case 0x43: gs.alpha[reg - 0x42] = v; break;
    case 0x44: gs.dimx = v; break;
    case 0x45: gs.dthe = v; break;
    case 0x46: gs.colclamp = v; break;
    case 0x47: case 0x48: gs.test[reg - 0x47] = v; break;
    case 0x49: gs.pabe = v; break;
    case 0x4a: case 0x4b: gs.fba[reg - 0x4a] = v; break;
    case 0x4c: case 0x4d: gs.frame[reg - 0x4c] = v; break;
    case 0x4e: case 0x4f: gs.zbuf[reg - 0x4e] = v; break;
    case 0x50: gs.bitbltbuf = v; break;
    case 0x51: gs.trxpos = v; break;
    case 0x52: gs.trxreg = v; break;
    case 0x53: gs.trxdir = v; xfer_start(); break;
    case 0x54: xfer_data(v); break;
    case 0x60: gs_signal(v); break;
    case 0x61: gs_finish(); break;
    case 0x62: gs.label = (gs.label & ~(v >> 32)) | (v & (v >> 32)); break;
    default: break;
    }
}

/* ---- GIF packets ------------------------------------------------------------------------ */
typedef struct {
    int nloop, flg, nreg, reg_i, eop, active;
    uint64_t regs;
    uint32_t q;   /* last Q from ST */
} GifPath;

static GifPath paths[3];

static void gif_packed(GifPath *p, int reg, const U128 *d) {
    uint64_t lo = d->d[0], hi = d->d[1];
    switch (reg) {
    case 0x0: gs_write_reg(0x00, lo & 0x7ff); break;
    case 0x1: {   /* RGBAQ */
        uint64_t v = (lo & 0xff) | (((lo >> 32) & 0xff) << 8) | ((hi & 0xff) << 16) | (((hi >> 32) & 0xff) << 24);
        v |= (uint64_t)p->q << 32;
        gs_write_reg(0x01, v);
        break;
    }
    case 0x2:   /* ST + Q */
        p->q = (uint32_t)hi;
        gs_write_reg(0x02, lo);
        break;
    case 0x3: gs_write_reg(0x03, (lo & 0x3fff) | (((lo >> 32) & 0x3fff) << 16)); break;
    case 0x4: {   /* XYZF2 / XYZF3 (ADC) */
        uint64_t x = lo & 0xffff, y = (lo >> 32) & 0xffff, z = (hi >> 4) & 0xffffff, f = (hi >> 36) & 0xff;
        int adc = (hi >> 47) & 1;
        gs_write_reg(adc ? 0x0c : 0x04, x | (y << 16) | (z << 32) | (f << 56));
        break;
    }
    case 0x5: {   /* XYZ2 / XYZ3 */
        uint64_t x = lo & 0xffff, y = (lo >> 32) & 0xffff, z = hi & 0xffffffff;
        int adc = (hi >> 47) & 1;
        gs_write_reg(adc ? 0x0d : 0x05, x | (y << 16) | (z << 32));
        break;
    }
    case 0x6: gs_write_reg(0x06, lo); break;
    case 0x7: gs_write_reg(0x07, lo); break;
    case 0x8: gs_write_reg(0x08, lo); break;
    case 0x9: gs_write_reg(0x09, lo); break;
    case 0xa: gs_write_reg(0x0a, ((hi >> 36) & 0xff) << 56); break;
    case 0xc: gs_write_reg(0x0c, lo); break;
    case 0xd: gs_write_reg(0x0d, lo); break;
    case 0xe: gs_write_reg((int)(hi & 0xff), lo); break;
    case 0xf: break;
    }
}

/* GPU mode: the GPU renderer parses the packets; here only SIGNAL/FINISH/LABEL (A+D in PACKED
   mode) matter, so whole packets without A+D and IMAGE data are skipped. */
static void gif_scan(GifPath *p, const U128 *q, uint32_t n) {
    uint32_t i = 0;
    while (i < n) {
        if (!p->active) {
            uint64_t tag = q[i].d[0];
            p->nloop = bits(tag, 0, 15);
            p->flg = bits(tag, 58, 2);
            p->nreg = bits(tag, 60, 4);
            if (!p->nreg) p->nreg = 16;
            p->regs = q[i].d[1];
            p->reg_i = 0;
            p->q = 0;   /* reused here: the packet has an A+D register */
            if (p->flg == 0)
                for (int r = 0; r < p->nreg; r++) if (((p->regs >> (4 * r)) & 15) == 0xe) p->q = 1;
            p->active = p->nloop > 0;
            i++;
            continue;
        }
        if (p->flg == 0 && p->q) {
            for (; i < n && p->active; i++) {
                if (((p->regs >> (4 * p->reg_i)) & 15) == 0xe) {
                    int reg = (int)(q[i].d[1] & 0xff);
                    if (reg >= 0x60 && reg <= 0x62) gs_write_reg(reg, q[i].d[0]);
                }
                if (++p->reg_i >= p->nreg) { p->reg_i = 0; if (--p->nloop == 0) p->active = 0; }
            }
            continue;
        }
        /* PACKED without A+D: nloop*nreg qwords; REGLIST: two registers per qword; IMAGE: nloop qwords */
        uint64_t left = p->flg == 0 ? (uint64_t)p->nloop * p->nreg - p->reg_i
                      : p->flg == 1 ? ((uint64_t)p->nloop * p->nreg - p->reg_i + 1) / 2 : (uint64_t)p->nloop;
        uint64_t take = left < n - i ? left : n - i;
        if (p->flg == 0) {
            uint64_t total = p->reg_i + take;
            p->nloop -= (int)(total / p->nreg);
            p->reg_i = (int)(total % p->nreg);
        } else if (p->flg == 1) {
            uint64_t regs = take * 2, total = p->reg_i + regs;
            uint64_t loops = total / p->nreg;
            if (loops >= (uint64_t)p->nloop) { p->nloop = 0; p->reg_i = 0; }
            else { p->nloop -= (int)loops; p->reg_i = (int)(total % p->nreg); }
        } else {
            p->nloop -= (int)take;
        }
        if (p->nloop <= 0) { p->active = 0; p->nloop = 0; }
        i += (uint32_t)take;
    }
}

/* Feed `n` quadwords of GIF data on path `pi` (0 = PATH1, 1 = PATH2, 2 = PATH3). */
void gif_feed(int pi, const U128 *q, uint32_t n) {
    if (g_gs_gpu) {
        static int full = -1;
        if (full < 0) full = getenv("HP_GSPARSE") != NULL;   /* keep the full register parse (debug logs) */
        gsb_gif(pi, q, n);
        if (!full) { cur_path = pi; gif_scan(&paths[pi], q, n); return; }
    }
    GifPath *p = &paths[pi];
    cur_path = pi;
    for (uint32_t i = 0; i < n; i++) {
        const U128 *d = &q[i];
        if (!p->active) {
            uint64_t tag = d->d[0];
            p->nloop = bits(tag, 0, 15);
            p->eop = bits(tag, 15, 1);
            int pre = bits(tag, 46, 1);
            p->flg = bits(tag, 58, 2);
            p->nreg = bits(tag, 60, 4);
            if (!p->nreg) p->nreg = 16;
            p->regs = d->d[1];
            p->reg_i = 0;
            p->q = 0x3f800000u;   /* Q = 1.0 at the start of each packet */
            if (pre && p->flg == 0) gs_write_reg(0x00, bits(tag, 47, 11));
            p->active = p->nloop > 0;
            continue;
        }
        switch (p->flg) {
        case 0: {   /* PACKED */
            int reg = (int)((p->regs >> (4 * p->reg_i)) & 15);
            gif_packed(p, reg, d);
            if (++p->reg_i >= p->nreg) { p->reg_i = 0; if (--p->nloop == 0) p->active = 0; }
            break;
        }
        case 1:     /* REGLIST: two 64-bit registers per quadword */
            for (int h = 0; h < 2 && p->active; h++) {
                int reg = (int)((p->regs >> (4 * p->reg_i)) & 15);
                uint64_t v = d->d[h];
                /* REGLIST uses the raw register addresses for 0x0..0xd */
                if (reg == 0xe) { /* A+D is not valid in REGLIST */ }
                else if (reg != 0xf) gs_write_reg(reg == 0x0 ? 0x00 : reg, v);
                if (++p->reg_i >= p->nreg) { p->reg_i = 0; if (--p->nloop == 0) p->active = 0; }
            }
            break;
        default:    /* IMAGE */
            xfer_data(d->d[0]);
            xfer_data(d->d[1]);
            if (--p->nloop == 0) p->active = 0;
            break;
        }
    }
}

int gif_path_idle(int pi) { return !paths[pi].active; }

void gs_init(void) {
    g_vram = calloc(1, 4u << 20);
    if (gsb_enabled()) g_gs_gpu = gsb_init();
    memset(&gs, 0, sizeof gs);
    gs.prmodecont = 1;   /* reset value: primitive attributes come from PRIM */
}
