/* IPU: the MPEG-2 macroblock decoder (IDEC/BDEC/VDEC/FDEC/CSC/PACK/SETIQ/...).
 * Register semantics follow the real unit as reconstructed by PCSX2; input
 * arrives through DMA toIPU or the FIFO, output leaves through DMA fromIPU. */
#include "rt.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* ---- VLC tables (ISO/IEC 13818-2 B.1-B.15, in the form PCSX2 uses) ------- */
static const uint16_t dc_lum_code[12] = {0x4, 0x0, 0x1, 0x5, 0x6, 0xe, 0x1e, 0x3e, 0x7e, 0xfe, 0x1fe, 0x1ff};
static const uint8_t dc_lum_bits[12] = {3, 2, 2, 3, 3, 4, 5, 6, 7, 8, 9, 9};
static const uint16_t dc_chroma_code[12] = {0x0, 0x1, 0x2, 0x6, 0xe, 0x1e, 0x3e, 0x7e, 0xfe, 0x1fe, 0x3fe, 0x3ff};
static const uint8_t dc_chroma_bits[12] = {2, 2, 2, 3, 4, 5, 6, 7, 8, 9, 10, 10};
#define NB_RL 111
static const uint16_t vlc_b14[NB_RL + 2][2] = {
    {0x3, 2}, {0x4, 4}, {0x5, 5}, {0x6, 7}, {0x26, 8}, {0x21, 8}, {0xa, 10}, {0x1d, 12},
    {0x18, 12}, {0x13, 12}, {0x10, 12}, {0x1a, 13}, {0x19, 13}, {0x18, 13}, {0x17, 13}, {0x1f, 14},
    {0x1e, 14}, {0x1d, 14}, {0x1c, 14}, {0x1b, 14}, {0x1a, 14}, {0x19, 14}, {0x18, 14}, {0x17, 14},
    {0x16, 14}, {0x15, 14}, {0x14, 14}, {0x13, 14}, {0x12, 14}, {0x11, 14}, {0x10, 14}, {0x18, 15},
    {0x17, 15}, {0x16, 15}, {0x15, 15}, {0x14, 15}, {0x13, 15}, {0x12, 15}, {0x11, 15}, {0x10, 15},
    {0x3, 3}, {0x6, 6}, {0x25, 8}, {0xc, 10}, {0x1b, 12}, {0x16, 13}, {0x15, 13}, {0x1f, 15},
    {0x1e, 15}, {0x1d, 15}, {0x1c, 15}, {0x1b, 15}, {0x1a, 15}, {0x19, 15}, {0x13, 16}, {0x12, 16},
    {0x11, 16}, {0x10, 16}, {0x5, 4}, {0x4, 7}, {0xb, 10}, {0x14, 12}, {0x14, 13}, {0x7, 5},
    {0x24, 8}, {0x1c, 12}, {0x13, 13}, {0x6, 5}, {0xf, 10}, {0x12, 12}, {0x7, 6}, {0x9, 10},
    {0x12, 13}, {0x5, 6}, {0x1e, 12}, {0x14, 16}, {0x4, 6}, {0x15, 12}, {0x7, 7}, {0x11, 12},
    {0x5, 7}, {0x11, 13}, {0x27, 8}, {0x10, 13}, {0x23, 8}, {0x1a, 16}, {0x22, 8}, {0x19, 16},
    {0x20, 8}, {0x18, 16}, {0xe, 10}, {0x17, 16}, {0xd, 10}, {0x16, 16}, {0x8, 10}, {0x15, 16},
    {0x1f, 12}, {0x1a, 12}, {0x19, 12}, {0x17, 12}, {0x16, 12}, {0x1f, 13}, {0x1e, 13}, {0x1d, 13},
    {0x1c, 13}, {0x1b, 13}, {0x1f, 16}, {0x1e, 16}, {0x1d, 16}, {0x1c, 16}, {0x1b, 16},
    {0x1, 6}, {0x2, 2}};
static const uint16_t vlc_b15[NB_RL + 2][2] = {
    {0x02, 2}, {0x06, 3}, {0x07, 4}, {0x1c, 5}, {0x1d, 5}, {0x05, 6}, {0x04, 6}, {0x7b, 7},
    {0x7c, 7}, {0x23, 8}, {0x22, 8}, {0xfa, 8}, {0xfb, 8}, {0xfe, 8}, {0xff, 8}, {0x1f, 14},
    {0x1e, 14}, {0x1d, 14}, {0x1c, 14}, {0x1b, 14}, {0x1a, 14}, {0x19, 14}, {0x18, 14}, {0x17, 14},
    {0x16, 14}, {0x15, 14}, {0x14, 14}, {0x13, 14}, {0x12, 14}, {0x11, 14}, {0x10, 14}, {0x18, 15},
    {0x17, 15}, {0x16, 15}, {0x15, 15}, {0x14, 15}, {0x13, 15}, {0x12, 15}, {0x11, 15}, {0x10, 15},
    {0x02, 3}, {0x06, 5}, {0x79, 7}, {0x27, 8}, {0x20, 8}, {0x16, 13}, {0x15, 13}, {0x1f, 15},
    {0x1e, 15}, {0x1d, 15}, {0x1c, 15}, {0x1b, 15}, {0x1a, 15}, {0x19, 15}, {0x13, 16}, {0x12, 16},
    {0x11, 16}, {0x10, 16}, {0x05, 5}, {0x07, 7}, {0xfc, 8}, {0x0c, 10}, {0x14, 13}, {0x07, 5},
    {0x26, 8}, {0x1c, 12}, {0x13, 13}, {0x06, 6}, {0xfd, 8}, {0x12, 12}, {0x07, 6}, {0x04, 9},
    {0x12, 13}, {0x06, 7}, {0x1e, 12}, {0x14, 16}, {0x04, 7}, {0x15, 12}, {0x05, 7}, {0x11, 12},
    {0x78, 7}, {0x11, 13}, {0x7a, 7}, {0x10, 13}, {0x21, 8}, {0x1a, 16}, {0x25, 8}, {0x19, 16},
    {0x24, 8}, {0x18, 16}, {0x05, 9}, {0x17, 16}, {0x07, 9}, {0x16, 16}, {0x0d, 10}, {0x15, 16},
    {0x1f, 12}, {0x1a, 12}, {0x19, 12}, {0x17, 12}, {0x16, 12}, {0x1f, 13}, {0x1e, 13}, {0x1d, 13},
    {0x1c, 13}, {0x1b, 13}, {0x1f, 16}, {0x1e, 16}, {0x1d, 16}, {0x1c, 16}, {0x1b, 16},
    {0x01, 6}, {0x06, 4}};
static const int8_t rl_level[NB_RL] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
    25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 1, 2, 3, 4, 5, 6, 7, 8,
    9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 1, 2, 3, 4, 5, 1, 2, 3, 4, 1, 2, 3, 1, 2,
    3, 1, 2, 3, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
static const int8_t rl_run[NB_RL] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 5, 5,
    5, 6, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16,
    17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
static const uint8_t mba_code[33][2] = {
    {0x1, 1}, {0x3, 3}, {0x2, 3}, {0x3, 4}, {0x2, 4}, {0x3, 5}, {0x2, 5}, {0x7, 7}, {0x6, 7},
    {0xb, 8}, {0xa, 8}, {0x9, 8}, {0x8, 8}, {0x7, 8}, {0x6, 8}, {0x17, 10}, {0x16, 10},
    {0x15, 10}, {0x14, 10}, {0x13, 10}, {0x12, 10}, {0x23, 11}, {0x22, 11}, {0x21, 11}, {0x20, 11},
    {0x1f, 11}, {0x1e, 11}, {0x1d, 11}, {0x1c, 11}, {0x1b, 11}, {0x1a, 11}, {0x19, 11}, {0x18, 11}};
static const uint8_t zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
static const uint8_t altscan[64] = {
    0, 8, 16, 24, 1, 9, 2, 10, 17, 25, 32, 40, 48, 56, 57, 49, 41, 33, 26, 18, 3, 11, 4, 12, 19, 27, 34, 42, 50, 58, 35, 43,
    51, 59, 20, 28, 5, 13, 6, 14, 21, 29, 36, 44, 52, 60, 37, 45, 53, 61, 22, 30, 7, 15, 23, 31, 38, 46, 54, 62, 39, 47, 55, 63};
/* default intra matrix in scan (zigzag) order, like the matrices SETIQ delivers */
static const uint8_t default_intra_scan[64] = {
    8, 16, 16, 19, 16, 19, 22, 22, 22, 22, 22, 22, 26, 24, 26, 27, 27, 27, 26, 26, 26, 26, 27, 27, 27, 29, 29, 29, 34, 34, 34, 29,
    29, 29, 27, 27, 29, 29, 32, 32, 34, 34, 37, 38, 37, 35, 35, 34, 35, 38, 38, 40, 40, 40, 48, 48, 46, 46, 56, 56, 58, 69, 69, 83};
static const uint8_t nonlinear_q[32] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 18, 20, 22, 24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96, 104, 112};

enum { MB_INTRA = 1, MB_PATTERN = 2, MB_BWD = 4, MB_FWD = 8, MB_QUANT = 16, MB_DCT_INTERLACED = 32, MC_FRAME = 128 };
#define INTRA MB_INTRA
#define QUANT MB_QUANT
#define CODED MB_PATTERN
#define MC MB_FWD
#define FWD MB_FWD
#define BWD MB_BWD
#define INTER (MB_FWD | MB_BWD)
static const uint8_t MB_P[32][2] = {
    {INTRA | QUANT, 6}, {CODED | QUANT, 5}, {MC | CODED | QUANT, 5}, {INTRA, 5},
    {MC, 3}, {MC, 3}, {MC, 3}, {MC, 3}, {CODED, 2}, {CODED, 2}, {CODED, 2}, {CODED, 2},
    {CODED, 2}, {CODED, 2}, {CODED, 2}, {CODED, 2}, {MC | CODED, 1}, {MC | CODED, 1}, {MC | CODED, 1}, {MC | CODED, 1},
    {MC | CODED, 1}, {MC | CODED, 1}, {MC | CODED, 1}, {MC | CODED, 1}, {MC | CODED, 1}, {MC | CODED, 1}, {MC | CODED, 1}, {MC | CODED, 1},
    {MC | CODED, 1}, {MC | CODED, 1}, {MC | CODED, 1}, {MC | CODED, 1}};
static const uint8_t MB_B[64][2] = {
    {0, 0}, {INTRA | QUANT, 6}, {BWD | CODED | QUANT, 6}, {FWD | CODED | QUANT, 6},
    {INTER | CODED | QUANT, 5}, {INTER | CODED | QUANT, 5}, {INTRA, 5}, {INTRA, 5},
    {FWD, 4}, {FWD, 4}, {FWD, 4}, {FWD, 4}, {FWD | CODED, 4}, {FWD | CODED, 4}, {FWD | CODED, 4}, {FWD | CODED, 4},
    {BWD, 3}, {BWD, 3}, {BWD, 3}, {BWD, 3}, {BWD, 3}, {BWD, 3}, {BWD, 3}, {BWD, 3},
    {BWD | CODED, 3}, {BWD | CODED, 3}, {BWD | CODED, 3}, {BWD | CODED, 3}, {BWD | CODED, 3}, {BWD | CODED, 3}, {BWD | CODED, 3}, {BWD | CODED, 3},
    {INTER, 2}, {INTER, 2}, {INTER, 2}, {INTER, 2}, {INTER, 2}, {INTER, 2}, {INTER, 2}, {INTER, 2},
    {INTER, 2}, {INTER, 2}, {INTER, 2}, {INTER, 2}, {INTER, 2}, {INTER, 2}, {INTER, 2}, {INTER, 2},
    {INTER | CODED, 2}, {INTER | CODED, 2}, {INTER | CODED, 2}, {INTER | CODED, 2}, {INTER | CODED, 2}, {INTER | CODED, 2}, {INTER | CODED, 2}, {INTER | CODED, 2},
    {INTER | CODED, 2}, {INTER | CODED, 2}, {INTER | CODED, 2}, {INTER | CODED, 2}, {INTER | CODED, 2}, {INTER | CODED, 2}, {INTER | CODED, 2}, {INTER | CODED, 2}};
static const uint8_t MV_4[8][2] = {{3, 6}, {2, 4}, {1, 3}, {1, 3}, {0, 2}, {0, 2}, {0, 2}, {0, 2}};
static const uint8_t MV_10[48][2] = {
    {0, 10}, {0, 10}, {0, 10}, {0, 10}, {0, 10}, {0, 10}, {0, 10}, {0, 10}, {0, 10}, {0, 10}, {0, 10}, {0, 10},
    {15, 10}, {14, 10}, {13, 10}, {12, 10}, {11, 10}, {10, 10}, {9, 9}, {9, 9}, {8, 9}, {8, 9}, {7, 9}, {7, 9},
    {6, 7}, {6, 7}, {6, 7}, {6, 7}, {6, 7}, {6, 7}, {6, 7}, {6, 7}, {5, 7}, {5, 7}, {5, 7}, {5, 7},
    {5, 7}, {5, 7}, {5, 7}, {5, 7}, {4, 7}, {4, 7}, {4, 7}, {4, 7}, {4, 7}, {4, 7}, {4, 7}, {4, 7}};
static const int8_t DMV_2[4][2] = {{0, 1}, {0, 1}, {1, 2}, {-1, 2}};
static const uint8_t CBP_7[112][2] = {
    {0x22, 7}, {0x12, 7}, {0x0a, 7}, {0x06, 7}, {0x21, 7}, {0x11, 7}, {0x09, 7}, {0x05, 7},
    {0x3f, 6}, {0x3f, 6}, {0x03, 6}, {0x03, 6}, {0x24, 6}, {0x24, 6}, {0x18, 6}, {0x18, 6},
    {0x3e, 5}, {0x3e, 5}, {0x3e, 5}, {0x3e, 5}, {0x02, 5}, {0x02, 5}, {0x02, 5}, {0x02, 5},
    {0x3d, 5}, {0x3d, 5}, {0x3d, 5}, {0x3d, 5}, {0x01, 5}, {0x01, 5}, {0x01, 5}, {0x01, 5},
    {0x38, 5}, {0x38, 5}, {0x38, 5}, {0x38, 5}, {0x34, 5}, {0x34, 5}, {0x34, 5}, {0x34, 5},
    {0x2c, 5}, {0x2c, 5}, {0x2c, 5}, {0x2c, 5}, {0x1c, 5}, {0x1c, 5}, {0x1c, 5}, {0x1c, 5},
    {0x28, 5}, {0x28, 5}, {0x28, 5}, {0x28, 5}, {0x14, 5}, {0x14, 5}, {0x14, 5}, {0x14, 5},
    {0x30, 5}, {0x30, 5}, {0x30, 5}, {0x30, 5}, {0x0c, 5}, {0x0c, 5}, {0x0c, 5}, {0x0c, 5},
    {0x20, 4}, {0x20, 4}, {0x20, 4}, {0x20, 4}, {0x20, 4}, {0x20, 4}, {0x20, 4}, {0x20, 4},
    {0x10, 4}, {0x10, 4}, {0x10, 4}, {0x10, 4}, {0x10, 4}, {0x10, 4}, {0x10, 4}, {0x10, 4},
    {0x08, 4}, {0x08, 4}, {0x08, 4}, {0x08, 4}, {0x08, 4}, {0x08, 4}, {0x08, 4}, {0x08, 4},
    {0x04, 4}, {0x04, 4}, {0x04, 4}, {0x04, 4}, {0x04, 4}, {0x04, 4}, {0x04, 4}, {0x04, 4},
    {0x3c, 3}, {0x3c, 3}, {0x3c, 3}, {0x3c, 3}, {0x3c, 3}, {0x3c, 3}, {0x3c, 3}, {0x3c, 3},
    {0x3c, 3}, {0x3c, 3}, {0x3c, 3}, {0x3c, 3}, {0x3c, 3}, {0x3c, 3}, {0x3c, 3}, {0x3c, 3}};
static const uint8_t CBP_9[64][2] = {
    {0, 0}, {0x00, 9}, {0x27, 9}, {0x1b, 9}, {0x3b, 9}, {0x37, 9}, {0x2f, 9}, {0x1f, 9},
    {0x3a, 8}, {0x3a, 8}, {0x36, 8}, {0x36, 8}, {0x2e, 8}, {0x2e, 8}, {0x1e, 8}, {0x1e, 8},
    {0x39, 8}, {0x39, 8}, {0x35, 8}, {0x35, 8}, {0x2d, 8}, {0x2d, 8}, {0x1d, 8}, {0x1d, 8},
    {0x26, 8}, {0x26, 8}, {0x1a, 8}, {0x1a, 8}, {0x25, 8}, {0x25, 8}, {0x19, 8}, {0x19, 8},
    {0x2b, 8}, {0x2b, 8}, {0x17, 8}, {0x17, 8}, {0x33, 8}, {0x33, 8}, {0x0f, 8}, {0x0f, 8},
    {0x2a, 8}, {0x2a, 8}, {0x16, 8}, {0x16, 8}, {0x32, 8}, {0x32, 8}, {0x0e, 8}, {0x0e, 8},
    {0x29, 8}, {0x29, 8}, {0x15, 8}, {0x15, 8}, {0x31, 8}, {0x31, 8}, {0x0d, 8}, {0x0d, 8},
    {0x23, 8}, {0x23, 8}, {0x13, 8}, {0x13, 8}, {0x0b, 8}, {0x0b, 8}, {0x07, 8}, {0x07, 8}};

/* ---- state ------------------------------------------------------------------ */
static uint8_t *in;            /* input bitstream bytes */
static uint32_t in_len, in_cap;
static uint64_t bitpos;        /* absolute bit position in `in` */
static int underflow;          /* a read went past the data received so far */
static U128 *out;              /* output FIFO */
static uint32_t out_len, out_rd, out_cap;
static uint32_t ctrl, th0, th1, cmd_result, top, last_cmd;
static int ecd, scd, cbp_reg, busy;
static uint8_t iq[64], nq[64];  /* quantizer matrices in scan order */
static uint16_t vqclut[16];
static void from_drain(void);

static void in_append(const uint8_t *p, uint32_t n) {
    if (in_len + n > in_cap) { in_cap = (in_len + n) * 2 + 4096; in = realloc(in, in_cap); }
    memcpy(in + in_len, p, n);
    in_len += n;
}

/* Input FIFO: the toIPU DMA is pulled so that at most FIFO_QW unread quadwords sit in front of
   the decoder (8 in the FIFO + 2 in the bit-unit register), as on hardware.  Games that switch
   to CSC mid-stream rewind D4_MADR by IFC+FP and restore BP afterwards, which only works if the
   DMA position is honest. */
#define FIFO_QW 10
int dma_ipu_pull(U128 *q);
static uint32_t unread_qw(void) {
    uint32_t used = (uint32_t)(bitpos >> 7), have = in_len / 16;
    return have > used ? have - used : 0;
}
static int pull_qw(uint32_t want) {   /* pull until `want` unread quadwords; returns how many were added */
    int got = 0;
    U128 q;
    while (unread_qw() < want && dma_ipu_pull(&q)) { in_append((const uint8_t *)&q, 16); got++; }
    return got;
}
static void out_push(const void *p, uint32_t nq) {
    if (out_len + nq > out_cap) { out_cap = (out_len + nq) * 2 + 1024; out = realloc(out, out_cap * sizeof(U128)); }
    memcpy(out + out_len, p, nq * sizeof(U128));
    out_len += nq;
}

static inline uint32_t peek(int n) {   /* next n bits (n <= 32), MSB first */
    if (bitpos + (uint64_t)n > (uint64_t)in_len * 8) underflow = 1;
    uint64_t v = 0;
    uint64_t byte = bitpos >> 3;
    for (int k = 0; k < 8; k++) v = (v << 8) | (byte + k < in_len ? in[byte + k] : 0);
    v <<= (bitpos & 7);
    return (uint32_t)(v >> (64 - n));
}
static inline int32_t speek(int n) { return (int32_t)(peek(n) << (32 - n)) >> (32 - n); }
static inline void skip(int n) { bitpos += (uint64_t)n; }
static inline uint32_t getb(int n) { uint32_t v = peek(n); skip(n); return v; }

unsigned g_ipu_vlc_errors, g_ipu_bdec_nooutput;
static int vlc_lookup(const uint16_t (*tab)[2], int n) {
    uint32_t w = peek(16);
    for (int k = 0; k < n; k++) {
        int len = tab[k][1];
        if ((w >> (16 - len)) == tab[k][0]) { skip(len); return k; }
    }
    g_ipu_vlc_errors++;
    return -1;
}

/* ---- decoder state ---------------------------------------------------------- */
static struct {
    int mpeg1, qst, ivf, as, idp, pct;
    int qscale, modes, cbp;
    int dc_pred[3];
    int16_t block[64];
    uint8_t mb8[384];      /* Y[16][16] Cb[8][8] Cr[8][8] */
    int16_t mb16[384];
    uint32_t rgb32[256];
} dec;

static void load_ctrl(void) {
    dec.idp = (ctrl >> 16) & 3; dec.as = (ctrl >> 20) & 1; dec.ivf = (ctrl >> 21) & 1;
    dec.qst = (ctrl >> 22) & 1; dec.mpeg1 = (ctrl >> 23) & 1; dec.pct = (ctrl >> 24) & 7;
}
static int qscale_of(int code) { return dec.qst ? nonlinear_q[code & 31] : (code & 31) << 1; }

static int get_dc(int chroma) {
    const uint16_t *code = chroma ? dc_chroma_code : dc_lum_code;
    const uint8_t *nb = chroma ? dc_chroma_bits : dc_lum_bits;
    uint32_t w = peek(16);
    int size = -1;
    for (int k = 0; k < 12; k++)
        if ((w >> (16 - nb[k])) == code[k]) { skip(nb[k]); size = k; break; }
    if (size <= 0) return 0;
    int v = (int)getb(size);
    if (!(v & (1 << (size - 1)))) v = v - (1 << size) + 1;
    return v;
}

/* macroblock_address_increment in the IPU's result format */
static uint32_t get_mbai(void) {
    uint32_t w11 = peek(11);
    if (w11 == 8) { skip(11); return 0xb0023; }                  /* escape */
    if (w11 == 15 && dec.mpeg1) { skip(11); return 0xb0022; }   /* stuffing */
    for (int k = 0; k < 33; k++) {
        int len = mba_code[k][1];
        if (peek(len) == mba_code[k][0]) { skip(len); return (uint32_t)(k + 1) | ((uint32_t)len << 16); }
    }
    return 0;
}

static uint32_t get_mb_modes(void) {
    switch (dec.pct ? dec.pct : 1) {
    case 1: {
        uint32_t c = peek(2);
        if (c == 0) return 0;
        if (c >> 1) { skip(1); return INTRA; }
        skip(2); return INTRA | QUANT;
    }
    case 2: {
        uint32_t c = peek(6);
        if (c == 0) return 0;
        const uint8_t *t = MB_P[c >> 1];
        skip(t[1]);
        uint32_t m = t[0];
        if (m & MB_FWD) m |= MC_FRAME;
        return m;
    }
    case 3: {
        uint32_t c = peek(6);
        if (c == 0) return 0;
        const uint8_t *t = MB_B[c];
        skip(t[1]);
        return (uint32_t)t[0] | MC_FRAME | ((uint32_t)t[1] << 16);
    }
    case 4:
        getb(1);
        return INTRA | (1u << 16);
    }
    return 0;
}

static inline int16_t sat12(int v) { return v > 2047 ? 2047 : v < -2048 ? -2048 : (int16_t)v; }

static int get_intra_block(void) {
    const uint8_t *scan = dec.as ? altscan : zigzag;
    const uint16_t (*tab)[2] = (dec.ivf && !dec.mpeg1) ? vlc_b15 : vlc_b14;
    int qs = dec.qscale;
    for (int i = 1; i < 64;) {
        int k = vlc_lookup(tab, NB_RL + 2);
        if (k < 0 || k == NB_RL + 1) return 0;   /* EOB or an unknown code */
        int run, val;
        if (k == NB_RL) {               /* escape */
            run = (int)getb(6);
            i += run;
            if (i >= 64) return 0;
            if (!dec.mpeg1) {
                val = (speek(12) * qs * iq[i]) >> 4;
                skip(12);
            } else {
                val = speek(8); skip(8);
                if (!(val & 0x7f)) val = (int)getb(8) + 2 * val;
                val = (val * qs * iq[i]) >> 4;
                val = (val + ~(val >> 31)) | 1;
            }
        } else {
            i += rl_run[k];
            if (i >= 64) return 0;
            val = (rl_level[k] * qs * iq[i]) >> 4;
            if (dec.mpeg1) val = (val - 1) | 1;
            int s = -(int)getb(1);
            val = (val ^ s) - s;
        }
        dec.block[scan[i]] = sat12(val);
        i++;
    }
    return 0;
}

static int get_non_intra_block(void) {
    const uint8_t *scan = dec.as ? altscan : zigzag;
    int qs = dec.qscale;
    for (int i = 0; i < 64;) {
        int k, run, val;
        if (i == 0 && peek(1) == 1) { skip(1); k = 0; }   /* first coefficient: "1" = run 0 level 1 */
        else {
            k = vlc_lookup(vlc_b14, NB_RL + 2);
            if (k < 0 || k == NB_RL + 1) return 0;   /* EOB or an unknown code */
        }
        if (k == NB_RL) {
            run = (int)getb(6);
            i += run;
            if (i >= 64) return 0;
            if (!dec.mpeg1) {
                int lv = speek(12);
                val = ((2 * (lv + (lv >> 31)) + 1) * qs * nq[i]) >> 5;
                skip(12);
            } else {
                val = speek(8); skip(8);
                if (!(val & 0x7f)) val = (int)getb(8) + 2 * val;
                val = ((2 * (val + (val >> 31)) + 1) * qs * nq[i]) / 32;
                val = (val + ~(val >> 31)) | 1;
            }
        } else {
            i += rl_run[k];
            if (i >= 64) return 0;
            val = ((2 * rl_level[k] + 1) * qs * nq[i]) >> 5;
            if (dec.mpeg1) val = (val + ~(val >> 31)) | 1;
            int s = -(int)getb(1);
            val = (val ^ s) - s;
        }
        dec.block[scan[i]] = sat12(val);
        i++;
    }
    return 0;
}

/* ---- IDCT (the integer transform PCSX2 reconstructed from the IPU) --------- */
#define W1 2841
#define W2 2676
#define W3 2408
#define W5 1609
#define W6 1108
#define W7 565
static inline void butterfly(int *t0, int *t1, int w0, int w1, int d0, int d1) {
    int tmp = w0 * (d0 + d1);
    *t0 = tmp + (w1 - w0) * d1;
    *t1 = tmp - (w1 + w0) * d0;
}

static void idct_block(int16_t *block) {
    for (int i = 0; i < 8; i++) {
        int16_t *r = block + 8 * i;
        if (!(r[1] | r[2] | r[3] | r[4] | r[5] | r[6] | r[7])) {
            int16_t v = (int16_t)(r[0] << 3);
            for (int k = 0; k < 8; k++) r[k] = v;
            continue;
        }
        int a0, a1, a2, a3, b0, b1, b2, b3, t0, t1, t2, t3;
        {
            int d0 = (r[0] << 11) + 128, d1 = r[1], d2 = r[2] << 11, d3 = r[3];
            t0 = d0 + d2; t1 = d0 - d2;
            butterfly(&t2, &t3, W6, W2, d3, d1);
            a0 = t0 + t2; a1 = t1 + t3; a2 = t1 - t3; a3 = t0 - t2;
        }
        {
            int d0 = r[4], d1 = r[5], d2 = r[6], d3 = r[7];
            butterfly(&t0, &t1, W7, W1, d3, d0);
            butterfly(&t2, &t3, W3, W5, d1, d2);
            b0 = t0 + t2; b3 = t1 + t3;
            t0 -= t2; t1 -= t3;
            b1 = ((t0 + t1) * 181) >> 8;
            b2 = ((t0 - t1) * 181) >> 8;
        }
        r[0] = (int16_t)((a0 + b0) >> 8); r[1] = (int16_t)((a1 + b1) >> 8);
        r[2] = (int16_t)((a2 + b2) >> 8); r[3] = (int16_t)((a3 + b3) >> 8);
        r[4] = (int16_t)((a3 - b3) >> 8); r[5] = (int16_t)((a2 - b2) >> 8);
        r[6] = (int16_t)((a1 - b1) >> 8); r[7] = (int16_t)((a0 - b0) >> 8);
    }
    for (int i = 0; i < 8; i++) {
        int16_t *c = block + i;
        int a0, a1, a2, a3, b0, b1, b2, b3, t0, t1, t2, t3;
        {
            int d0 = (c[0] << 11) + 65536, d1 = c[8], d2 = c[16] << 11, d3 = c[24];
            t0 = d0 + d2; t1 = d0 - d2;
            butterfly(&t2, &t3, W6, W2, d3, d1);
            a0 = t0 + t2; a1 = t1 + t3; a2 = t1 - t3; a3 = t0 - t2;
        }
        {
            int d0 = c[32], d1 = c[40], d2 = c[48], d3 = c[56];
            butterfly(&t0, &t1, W7, W1, d3, d0);
            butterfly(&t2, &t3, W3, W5, d1, d2);
            b0 = t0 + t2; b3 = t1 + t3;
            t0 = (t0 - t2) >> 8; t1 = (t1 - t3) >> 8;
            b1 = (t0 + t1) * 181; b2 = (t0 - t1) * 181;
        }
        c[0] = (int16_t)((a0 + b0) >> 17); c[8] = (int16_t)((a1 + b1) >> 17);
        c[16] = (int16_t)((a2 + b2) >> 17); c[24] = (int16_t)((a3 + b3) >> 17);
        c[32] = (int16_t)((a3 - b3) >> 17); c[40] = (int16_t)((a2 - b2) >> 17);
        c[48] = (int16_t)((a1 - b1) >> 17); c[56] = (int16_t)((a0 - b0) >> 17);
    }
}

static void idct_copy(uint8_t *dst, int stride) {
    idct_block(dec.block);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int v = dec.block[y * 8 + x];
            dst[y * stride + x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    memset(dec.block, 0, sizeof dec.block);
}
static void idct_store16(int16_t *dst, int stride) {
    idct_block(dec.block);
    for (int y = 0; y < 8; y++) memcpy(dst + y * stride, dec.block + y * 8, 16);
    memset(dec.block, 0, sizeof dec.block);
}

static int intra_block(int cc, uint8_t *dst, int stride) {
    int diff = cc == 0 ? get_dc(0) : get_dc(1);
    dec.dc_pred[cc] += diff;
    dec.block[0] = (int16_t)(dec.dc_pred[cc] << (3 - dec.idp));
    if (get_intra_block() < 0) return -1;
    idct_copy(dst, stride);
    return 0;
}

/* Decode the six intra blocks of a macroblock into mb8. */
static int decode_intra_mb(void) {
    int off, stride;
    if (dec.modes & MB_DCT_INTERLACED) { off = 16; stride = 32; } else { off = 128; stride = 16; }
    uint8_t *Y = dec.mb8, *Cb = dec.mb8 + 256, *Cr = dec.mb8 + 320;
    if (intra_block(0, Y, stride) < 0) return -1;
    if (intra_block(0, Y + 8, stride) < 0) return -1;
    if (intra_block(0, Y + off, stride) < 0) return -1;
    if (intra_block(0, Y + off + 8, stride) < 0) return -1;
    if (intra_block(1, Cb, 8) < 0) return -1;
    if (intra_block(2, Cr, 8) < 0) return -1;
    return 0;
}

/* ---- colour conversion -------------------------------------------------------- */
static void yuv_to_rgb32(const uint8_t *mb8, uint32_t *rgb, int sgn) {
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            int Y = mb8[y * 16 + x], cb = mb8[256 + (y >> 1) * 8 + (x >> 1)], cr = mb8[320 + (y >> 1) * 8 + (x >> 1)];
            int lum = (0x95 * (Y - 16 > 0 ? Y - 16 : 0)) >> 6;
            int rcr = (0xcc * (cr - 128)) >> 6, gcr = (-0x68 * (cr - 128)) >> 6;
            int gcb = (-0x32 * (cb - 128)) >> 6, bcb = (0x102 * (cb - 128)) >> 6;
            int r = (lum + rcr + 1) >> 1, g = (lum + gcr + gcb + 1) >> 1, b = (lum + bcb + 1) >> 1;
            r = r < 0 ? 0 : r > 255 ? 255 : r;
            g = g < 0 ? 0 : g > 255 ? 255 : g;
            b = b < 0 ? 0 : b > 255 ? 255 : b;
            uint32_t a = 0x80, px;
            if (th0 > 0 && r < (int)th0 && g < (int)th0 && b < (int)th0) px = 0;
            else {
                if (th1 > 0 && r < (int)th1 && g < (int)th1 && b < (int)th1) a = 0x40;
                px = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | (a << 24);
            }
            if (sgn) px ^= 0x808080;
            rgb[y * 16 + x] = px;
        }
}
static void rgb32_to_rgb16(const uint32_t *rgb, uint16_t *out16) {
    for (int k = 0; k < 256; k++) {
        uint32_t c = rgb[k];
        out16[k] = (uint16_t)(((c & 0xff) >> 3) | (((c >> 8) & 0xff) >> 3 << 5) | (((c >> 16) & 0xff) >> 3 << 10) | ((c >> 31) << 15));
    }
}
static void emit_rgb(int ofm) {
    if (!ofm) { out_push(dec.rgb32, 64); return; }
    uint16_t p16[256];
    rgb32_to_rgb16(dec.rgb32, p16);
    out_push(p16, 32);
}

/* ---- commands ------------------------------------------------------------------- */
static void cmd_idec(uint32_t v) {
    skip(v & 63);
    load_ctrl();
    dec.pct = 1;
    dec.qscale = qscale_of((v >> 16) & 31);
    int dtd = (v >> 24) & 1, sgn = (v >> 25) & 1, ofm = (v >> 27) & 1;
    dec.dc_pred[0] = dec.dc_pred[1] = dec.dc_pred[2] = 128 << dec.idp;
    int mbs = 0;
    for (;;) {
        uint32_t modes = get_mb_modes();
        if (underflow) return;
        if (!modes) break;
        dec.modes = (int)modes;
        if (dtd && (modes & INTRA)) dec.modes |= getb(1) ? MB_DCT_INTERLACED : 0;
        if (modes & QUANT) dec.qscale = qscale_of((int)getb(5));
        memset(dec.mb8, 0, sizeof dec.mb8);
        if (decode_intra_mb() < 0 || underflow) return;
        yuv_to_rgb32(dec.mb8, dec.rgb32, sgn);
        emit_rgb(ofm);
        mbs++;
        uint32_t inc = get_mbai();
        if (underflow) return;
        if (!inc) break;
        if ((inc & 0xffff) > 1) dec.dc_pred[0] = dec.dc_pred[1] = dec.dc_pred[2] = 128 << dec.idp;
    }
    if (g_trace) LOG("[ipu] IDEC decoded %d macroblocks, %u output qwords\n", mbs, out_len - out_rd);
}

static void cmd_bdec(uint32_t v) {
    skip(v & 63);
    load_ctrl();
    dec.qscale = qscale_of((v >> 16) & 31);
    int dt = (v >> 25) & 1, dcr = (v >> 26) & 1, mbi = (v >> 27) & 1;
    dec.modes = (dt ? MB_DCT_INTERLACED : 0) | (mbi ? INTRA : MB_PATTERN);
    if (dcr) dec.dc_pred[0] = dec.dc_pred[1] = dec.dc_pred[2] = 128 << dec.idp;
    memset(dec.mb8, 0, sizeof dec.mb8);
    memset(dec.mb16, 0, sizeof dec.mb16);
    top = 0;
    if (mbi) {
        dec.cbp = 0x3f;
        if (decode_intra_mb() < 0 || underflow) return;
        for (int k = 0; k < 384; k++) dec.mb16[k] = dec.mb8[k];
    } else {
        uint32_t code = peek(16);
        const uint8_t *t = code >= 0x2000 ? CBP_7[peek(7) - 16] : CBP_9[peek(9)];
        skip(t[1]);
        dec.cbp = t[0];
        int off, stride;
        if (dec.modes & MB_DCT_INTERLACED) { off = 16; stride = 32; } else { off = 128; stride = 16; }
        int16_t *Y = dec.mb16, *Cb = dec.mb16 + 256, *Cr = dec.mb16 + 320;
        memset(dec.block, 0, sizeof dec.block);
        struct { int bit; int16_t *dst; int stride; } blocks[6] = {
            {0x20, Y, stride}, {0x10, Y + 8, stride}, {0x08, Y + off, stride}, {0x04, Y + off + 8, stride}, {0x02, Cb, 8}, {0x01, Cr, 8}};
        for (int b = 0; b < 6; b++) {
            if (!(dec.cbp & blocks[b].bit)) continue;
            if (get_non_intra_block() < 0 || underflow) return;
            idct_store16(blocks[b].dst, blocks[b].stride);
        }
    }
    if (underflow) return;
    scd = 0;
    cbp_reg = dec.cbp;
    out_push(dec.mb16, 48);
}
static unsigned bdec_count, vdec_count, csc_count, pack_count, idec_count;
static unsigned long long out_qw_total, out_qw_nonzero;

static void cmd_vdec(uint32_t v) {
    skip(v & 63);
    load_ctrl();
    uint32_t r = 0;
    switch ((v >> 26) & 3) {
    case 0: r = get_mbai(); break;
    case 1: r = get_mb_modes(); break;
    case 2: {
        uint32_t code = peek(16);
        if (code & 0x8000) { skip(1); r = 0x00010000; }
        else {
            const uint8_t *t = ((code & 0xf000) || ((code & 0xfc00) == 0x0c00)) ? MV_4[peek(4)] : MV_10[peek(10)];
            int delta = t[0] + 1;
            skip(t[1]);
            int sign = -(int)getb(1);
            r = (uint32_t)(((delta ^ sign) - sign) | (t[1] << 16));
        }
        break;
    }
    case 3: {
        const int8_t *t = DMV_2[peek(2)];
        skip(t[1]);
        r = (uint32_t)((int)t[0] | (t[1] << 16));
        break;
    }
    }
    if (underflow) return;
    cmd_result = r;
    ecd = (r == 0);
    top = peek(32);
}

static void cmd_csc(uint32_t v) {
    uint32_t mbc = v & 0x7ff;
    int ofm = (v >> 27) & 1;
    for (uint32_t m = 0; m < mbc; m++) {
        for (int k = 0; k < 384; k++) dec.mb8[k] = (uint8_t)getb(8);
        if (underflow) return;
        yuv_to_rgb32(dec.mb8, dec.rgb32, 0);
        emit_rgb(ofm);
    }
}

static void cmd_pack(uint32_t v) {
    uint32_t mbc = v & 0x7ff;
    int ofm = (v >> 27) & 1;
    for (uint32_t m = 0; m < mbc; m++) {
        for (int k = 0; k < 256; k++) {
            uint32_t px = 0;
            for (int j = 0; j < 4; j++) px |= getb(8) << (8 * j);
            dec.rgb32[k] = px;
        }
        if (underflow) return;
        uint16_t p16[256];
        rgb32_to_rgb16(dec.rgb32, p16);
        if (ofm) { out_push(p16, 32); continue; }
        uint8_t idx[128];
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 8; x++) {
                uint8_t best[2];
                for (int h = 0; h < 2; h++) {
                    uint16_t c = p16[y * 16 + 2 * x + h];
                    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31, bi = 0, bd = 1 << 30;
                    for (int k = 0; k < 16; k++) {
                        int dr = r - (vqclut[k] & 31), dg = g - ((vqclut[k] >> 5) & 31), db = b - ((vqclut[k] >> 10) & 31);
                        int d = dr * dr + dg * dg + db * db;
                        if (d < bd) { bd = d; bi = k; }
                    }
                    best[h] = (uint8_t)bi;
                }
                idx[y * 8 + x] = (uint8_t)((best[1] << 4) | best[0]);
            }
        out_push(idx, 8);
    }
}

static uint32_t pend_cmd;
static uint64_t cmd_bitpos;
static uint32_t cmd_outlen;
static int pending;
/* Everything a command mutates is snapshotted so a retry after underflow
 * starts from the same state (DC predictors, quantizer, matrices, CLUT). */
static struct { uint32_t th0, th1, cmd_result, top; int ecd, scd, cbp_reg; uint8_t iq[64], nq[64]; uint16_t vqclut[16]; } snap;
static __typeof__(dec) dec_snap;
static void snapshot(void) {
    dec_snap = dec;
    snap.th0 = th0; snap.th1 = th1; snap.cmd_result = cmd_result; snap.top = top;
    snap.ecd = ecd; snap.scd = scd; snap.cbp_reg = cbp_reg;
    memcpy(snap.iq, iq, 64); memcpy(snap.nq, nq, 64); memcpy(snap.vqclut, vqclut, sizeof vqclut);
}
static void restore(void) {
    dec = dec_snap;
    th0 = snap.th0; th1 = snap.th1; cmd_result = snap.cmd_result; top = snap.top;
    ecd = snap.ecd; scd = snap.scd; cbp_reg = snap.cbp_reg;
    memcpy(iq, snap.iq, 64); memcpy(nq, snap.nq, 64); memcpy(vqclut, snap.vqclut, sizeof vqclut);
}

static unsigned pic_bdec, pic_err;
static void exec_cmd(uint32_t v) {
    switch (v >> 28) {
    case 0: in_len = 0; bitpos = v & 127; break;   /* BCLR: clears the input FIFO, BP = v */
    case 1: cmd_idec(v); break;
    case 2: { uint32_t before = out_len; cmd_bdec(v); if (!underflow) { if (out_len == before) g_ipu_bdec_nooutput++; bdec_count++; pic_bdec++; } break; }
    case 3: cmd_vdec(v); break;
    case 4: skip(v & 63); cmd_result = peek(32); top = cmd_result; break;   /* FDEC */
    case 5: {   /* SETIQ: 64 bytes in scan order, IQM bit 27 selects non-intra */
        skip(v & 63);
        uint8_t *m = (v >> 27) & 1 ? nq : iq;
        for (int k = 0; k < 64; k++) m[k] = (uint8_t)getb(8);
        break;
    }
    case 6: for (int k = 0; k < 16; k++) { uint32_t lo = getb(8), hi = getb(8); vqclut[k] = (uint16_t)(lo | (hi << 8)); } break;   /* SETVQ */
    case 7: cmd_csc(v); if (!underflow) { csc_count++;
        if (getenv("HP_IPUPIC")) LOG("[ipu] CSC %u MBs after %u BDEC, %u VLC errors, ctrl %08x\n", v & 0x7ff, pic_bdec, g_ipu_vlc_errors - pic_err, ctrl);
        pic_bdec = 0; pic_err = g_ipu_vlc_errors; } break;
    case 8: pack_count++; cmd_pack(v); break;
    case 9: th0 = v & 0x1ff; th1 = (v >> 16) & 0x1ff; break;                       /* SETTH */
    default: {
        static int n;
        if (n++ < 5) LOG("[ipu] command %u (%08x) not implemented\n", v >> 28, v);
        break;
    }
    }
}

/* Run (or re-run) the command; it stays pending while the input runs dry. */
static void run_cmd(void) {
    for (;;) {
        bitpos = cmd_bitpos;
        out_len = cmd_outlen;
        restore();
        if ((pend_cmd >> 28) != 0) pull_qw(FIFO_QW);   /* the FIFO is kept full while the DMA runs */
        underflow = 0;
        exec_cmd(pend_cmd);
        if (!underflow) break;
        /* the command needs more than the FIFO holds: the hardware would have consumed
           bits meanwhile, so pull further and re-run from the snapshot */
        bitpos = cmd_bitpos;
        if (!pull_qw(unread_qw() + 8)) { pending = 1; busy = 1; return; }
    }
    pending = 0; busy = 0;
    pull_qw(FIFO_QW);
    from_drain();
}

static void compact_in(void);
void ipu_write_cmd(uint32_t v) {
    if (g_trace && (v >> 28) == 7) LOG("[ipu] CSC %08x\n", v);
    compact_in();
    last_cmd = v;
    pend_cmd = v;
    cmd_bitpos = bitpos;
    cmd_outlen = out_len;
    ecd = scd = 0;
    snapshot();
    run_cmd();
}

void ipu_write_ctrl(uint32_t v) {
    if (g_trace > 1 || getenv("HP_IPUCTRL")) { static uint32_t last = ~0u; if (v != last) { LOG("[ipu] CTRL <- %08x\n", v); last = v; } }
    if (v & 0x40000000u) {   /* RST */
        in_len = 0; bitpos = 0; out_len = out_rd = 0; pending = 0; busy = 0; underflow = 0;
        ecd = scd = cbp_reg = 0;
    }
    ctrl = v & 0x07f30000u;
}

void ipu_stats(void) {
    LOG("[stats] ipu: bdec %u csc %u pack %u vlc errors %u bdec without output %u, in %u bytes bitpos %llu pending %d, out qw %llu nonzero %llu\n", bdec_count, csc_count, pack_count,
        g_ipu_vlc_errors, g_ipu_bdec_nooutput, in_len, (unsigned long long)bitpos, pending, out_qw_total, out_qw_nonzero);
}

uint32_t ipu_read(uint32_t off) {
    uint32_t cmdtype = last_cmd >> 28;
    uint32_t un = unread_qw(), fp = un < 2 ? un : 2, ifc = un - fp;
    if (ifc > 8) ifc = 8;
    switch (off) {
    case 0x00: {                                                               /* CMD.DATA */
        if (cmdtype == 3 || cmdtype == 4) return cmd_result;
        int u = underflow; uint32_t t = peek(32); underflow = u; return t;
    }
    case 0x04: return busy ? 0x80000000u : 0;                                  /* CMD.BUSY */
    case 0x10: {                                                               /* CTRL */
        uint32_t ofc = out_len - out_rd;
        return ctrl | ifc | ((ofc > 8 ? 8 : ofc) << 4) | ((uint32_t)cbp_reg << 8) |
               (ecd ? 1u << 14 : 0) | (scd ? 1u << 15 : 0) | (busy ? 0x80000000u : 0);
    }
    case 0x20:                                                                 /* BP: BP, IFC, FP */
        return (uint32_t)(bitpos & 127) | (ifc << 8) | (fp << 16);
    case 0x30: { int u = underflow; uint32_t t = peek(32); underflow = u; return t; }   /* TOP */
    case 0x34: return busy ? 0x80000000u : 0;
    }
    return 0;
}

/* ---- DMA hooks -------------------------------------------------------------- */
static uint32_t from_madr, from_qwc;
static int from_active;
void dma_finish(int ch);
extern uint8_t *g_spr;

static void from_drain(void) {
    while (from_active && from_qwc && out_rd < out_len) {
        uint32_t a = from_madr;
        uint8_t *dst = ((a & 0x80000000u) || (a >> 28) == 7) ? g_spr + (a & 0x3ff0) : g_ram + (a & 0x01fffff0u);
        out_qw_total++; if (out[out_rd].d[0] | out[out_rd].d[1]) out_qw_nonzero++;
        memcpy(dst, &out[out_rd++], 16);
        from_madr += 16;
        from_qwc--;
    }
    if (out_rd == out_len) out_rd = out_len = 0;
    if (from_active && !from_qwc) { from_active = 0; dma_finish(3); }
}
void ipu_from_dma_start(uint32_t madr, uint32_t qwc) {
    { static long tl = -2; if (tl == -2) tl = getenv("HP_TAGLOG") ? atol(getenv("HP_TAGLOG")) : -1;
      extern unsigned long long g_vsyncs;
      if (tl >= 0 && (long)g_vsyncs >= tl && (long)g_vsyncs < tl + 60) LOG("[fromipu v%llu] madr %08x qwc %u (out avail %u)\n", g_vsyncs, madr, qwc, out_len - out_rd); }
    from_madr = madr; from_qwc = qwc; from_active = 1;
    from_drain();
}

/* drop input already consumed by completed commands */
static void compact_in(void) {
    if (pending || (bitpos >> 3) < 65536) return;
    uint32_t drop = (uint32_t)(bitpos >> 3) & ~15u;
    if (drop > in_len) drop = in_len & ~15u;
    memmove(in, in + drop, in_len - drop);
    in_len -= drop;
    bitpos -= (uint64_t)drop * 8;
}
void ipu_in_qwords(const uint8_t *p, uint32_t nq) {   /* EE writes to the IPU input FIFO */
    compact_in();
    in_append(p, nq * 16);
    if (pending) run_cmd();
}
void ipu_in_kick(void) {   /* toIPU DMA started */
    if (pending) { run_cmd(); return; }
    compact_in();
    pull_qw(FIFO_QW);
}
uint32_t ipu_out_qwords(uint8_t *dst, uint32_t nq) {
    uint32_t avail = out_len - out_rd, n = nq < avail ? nq : avail;
    memcpy(dst, out + out_rd, n * sizeof(U128));
    for (uint32_t k = 0; k < n; k++) { out_qw_total++; if (((U128 *)(out + out_rd))[k].d[0] | ((U128 *)(out + out_rd))[k].d[1]) out_qw_nonzero++; }
    out_rd += n;
    if (out_rd == out_len) out_rd = out_len = 0;
    return n;
}

void ipu_init(void) {
    memcpy(iq, default_intra_scan, 64);
    memset(nq, 16, 64);
}
