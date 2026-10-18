/* Runtime-internal declarations shared by core, kernel and hardware code. */
#pragma once
#include "ee_gen.h"
#include <stdio.h>
#include <pthread.h>

#define RAM_SIZE   0x02000000u
#define IMAGE_BASE 0x00100000u

#define LOG(...) do { fprintf(stderr, __VA_ARGS__); } while (0)

extern uint8_t *g_spr, *g_vumem;
extern char g_disc_dir[1024];
extern int g_trace;

uint8_t *gptr(uint32_t a);                       /* host pointer for guest RAM/SPR (never NULL) */
static inline uint32_t g32(uint32_t a) { return ld32(a); }
static inline void s32(uint32_t a, uint32_t v) { st32(a, v); }
static inline const char *gstr(uint32_t a) { return a ? (const char *)gptr(a) : NULL; }

GuestFn rt_lookup(uint32_t addr);
const char *rt_func_name(uint32_t addr);
void rt_dump(EE *c);
void rt_fatal(EE *c, const char *why) __attribute__((noreturn));

/* kernel.c */
void kern_run_main(uint32_t entry) __attribute__((noreturn));
EE *kern_current_ctx(void);
void kern_raise_intc(int line);                 /* hardware -> INTC (any host thread) */
void kern_raise_dmac(int ch);                   /* DMA channel finished */
uint32_t kern_intc_stat(void); void kern_intc_ack(uint32_t bits);
uint32_t kern_intc_mask(void); void kern_intc_toggle_mask(uint32_t bits);

/* hw.c */
void hw_init(void);
void hw_vblank(int start);                      /* called from the vblank timer */

/* iop.c (high-level IOP) */
void iop_init(void);
void sif_dma_to_iop(uint32_t src, uint32_t dst, uint32_t size, uint32_t attr);
