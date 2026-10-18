/* DMA controller channels and the VIF/GIF/IPU FIFOs (stubs until the GS work). */
#include "rt.h"

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

void dma_stats(void) {
    static unsigned last[10];
    LOG("[stats] dma/2s:");
    for (int k = 0; k < 10; k++) { LOG(" %s=%u", chname[k], g_dma_count[k] - last[k]); last[k] = g_dma_count[k]; }
    LOG("\n");
}

void dma_start(int ch) {
    static int seen[10];
    g_dma_count[ch]++;
    if (!seen[ch]++) LOG("[dma] first transfer on %s\n", chname[ch]);
    if (ch == 5) return;   /* SIF0 is armed by the game and fed by the IOP */
    if (ch == 8 || ch == 9) spr_transfer(ch);
    uint32_t chcr = mmio_r32(base[ch]);
    /* finish immediately: clear STR and QWC, signal completion */
    extern void mmio_w32_raw(uint32_t a, uint32_t v);
    mmio_w32_raw(base[ch], chcr & ~0x100u);
    mmio_w32_raw(base[ch] + 0x20, 0);
    kern_raise_dmac(ch);
}

void fifo_write(int which, U128 v) {
    static int seen[4];
    if (!seen[which]++) LOG("[fifo] first write to FIFO %d\n", which);
}

void gs_priv_write(uint32_t off, uint64_t v) {
    if (g_trace) LOG("[gs] priv %04x = %016llx\n", off, (unsigned long long)v);
}
