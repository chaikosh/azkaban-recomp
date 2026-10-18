/* Definitions only the generated code and the dispatcher need. */
#pragma once
#include "ee.h"

typedef struct { uint32_t addr; GuestFn fn; } FuncEntry;
extern const FuncEntry g_func_table[];
extern const uint32_t g_func_count;
extern const uint32_t g_entry_point;

#ifdef RA_CHECK
#define RA_ENTER() uint32_t ra0_ = RW(31)
#define RA_LEAVE(pc) do { if (UNLIKELY(RW(31) != ra0_)) rt_ra_mismatch(c, pc, ra0_); } while (0)
#else
#define RA_ENTER() ((void)0)
#define RA_LEAVE(pc) ((void)0)
#endif

#ifdef TRACE_PC
#define BB(a) (c->pc = (a), c->pcring[c->pcidx++ & 511] = (a))
#else
#define BB(a) ((void)0)
#endif
