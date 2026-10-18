/* Memory map, function dispatch, diagnostics and the program entry. */
#include "rt.h"
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <sys/mman.h>
#include <execinfo.h>
#include <unistd.h>

uint8_t *g_pt[65536];
uint8_t *g_ram;
uint8_t *g_spr;
uint8_t *g_vumem;            /* 64 KiB page at 0x11000000: VU0/VU1 micro and data memory */
uint8_t *g_vu0_mem;
volatile int g_irq_pending;
char g_disc_dir[1024] = "disc";
int g_trace;

static GuestFn *g_fast;      /* direct table over .text, indexed by (addr - text_base) / 4 */
static uint32_t g_text_lo, g_text_hi;

static void map_pages(uint32_t va, uint8_t *host, uint32_t size) {
    for (uint32_t off = 0; off < size; off += 0x10000)
        g_pt[(va + off) >> 16] = host + off;
}

static void mem_init(void) {
    g_ram = mmap(NULL, RAM_SIZE + 0x10000, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    g_spr = calloc(1, 0x10000);
    g_vumem = calloc(1, 0x10000);
    if (g_ram == MAP_FAILED || !g_spr || !g_vumem) { LOG("out of memory\n"); exit(1); }
    /* main RAM and its cached/uncached/accelerated mirrors */
    static const uint32_t mirrors[] = {0x00000000u, 0x20000000u, 0x30000000u, 0x80000000u, 0xA0000000u};
    for (unsigned k = 0; k < sizeof mirrors / sizeof *mirrors; k++)
        map_pages(mirrors[k], g_ram, RAM_SIZE);
    map_pages(0x70000000u, g_spr, 0x10000);   /* 16 KiB scratchpad, mirrored inside the page */
    map_pages(0x11000000u, g_vumem, 0x10000);
    g_vu0_mem = g_vumem + 0x4000;
    extern void vu1_init(uint8_t *);
    vu1_init(g_vumem);
}

uint8_t *gptr(uint32_t a) {
    uint8_t *p = HP(a);
    if (!p) { LOG("[mem] host pointer to unmapped %08x\n", a); return g_ram; }
    return p;
}

/* ---- dispatch ----------------------------------------------------------- */
static int cmp_fe(const void *a, const void *b) {
    uint32_t x = ((const FuncEntry *)a)->addr, y = ((const FuncEntry *)b)->addr;
    return x < y ? -1 : x > y;
}

static void dispatch_init(void) {
    g_text_lo = g_func_table[0].addr;
    g_text_hi = g_func_table[g_func_count - 1].addr + 4;
    g_fast = calloc((g_text_hi - g_text_lo) / 4, sizeof(GuestFn));
    for (uint32_t k = 0; k < g_func_count; k++)
        g_fast[(g_func_table[k].addr - g_text_lo) / 4] = g_func_table[k].fn;
}

GuestFn rt_lookup(uint32_t addr) {
    addr &= 0x1FFFFFFFu;
    if (addr >= g_text_lo && addr < g_text_hi && !(addr & 3))
        return g_fast[(addr - g_text_lo) / 4];
    FuncEntry key = {addr, 0};
    const FuncEntry *e = bsearch(&key, g_func_table, g_func_count, sizeof key, cmp_fe);
    return e ? e->fn : NULL;
}

const char *rt_func_name(uint32_t addr) {
    static __thread char buf[32];
    /* nearest function at or below addr */
    uint32_t lo = 0, hi = g_func_count;
    while (hi - lo > 1) { uint32_t m = (lo + hi) / 2; if (g_func_table[m].addr <= addr) lo = m; else hi = m; }
    snprintf(buf, sizeof buf, "f_%08x+%x", g_func_table[lo].addr, addr - g_func_table[lo].addr);
    return buf;
}

static uint32_t g_calltrace_ra;
uint32_t g_watch = 0xffffffffu;

extern unsigned long long g_vsyncs;
void rt_watch_hit(uint32_t a, uint64_t v, int size) {
    static int n;
    EE *c = kern_current_ctx();
    if (n++ < 40) LOG("[watch v%llu tick %u] write%d %08x = %llx at %s (thread %u, ra %08x)\n", g_vsyncs, ld32(0x483390u), size * 8, a, (unsigned long long)v,
                      c ? rt_func_name(c->pc) : "?", c ? c->thread : 0, c ? RW(31) : 0);
    static const char *wr_val; if (!wr_val) wr_val = getenv("HP_WATCHRING") ? getenv("HP_WATCHRING") : "";
    if (c && *wr_val && v == strtoull(wr_val, NULL, 16)) {   /* call chain leading here: last ring entries, function changes only */
        char line[4096]; int k = 0; char last[64] = "";
        uint32_t end = c->pcidx;
        for (uint32_t i = end > 4000 ? end - 4000 : 0; i < end; i++) {
            char fn[64]; snprintf(fn, sizeof fn, "%s", rt_func_name(c->pcring[i & 4095])); char *pl = strchr(fn, '+'); if (pl) *pl = 0;
            if (!strcmp(fn, last)) continue;
            snprintf(last, sizeof last, "%s", fn);
            k += snprintf(line + k, sizeof line - k, " %s", rt_func_name(c->pcring[i & 4095]));
            if (k > 3800) { LOG("[watchring]%s\n", line); k = 0; }
        }
        LOG("[watchring]%s\n", line);
    }
}

/* HP_CALLHIST=<vsync>: histogram of indirect call targets during the 120 vsyncs from there */
static long g_callhist_start = -1;
static struct { uint32_t addr; unsigned n; } callhist[4096];
extern unsigned long long g_vsyncs;
void rt_callhist_dump(void) {
    for (int pass = 0; pass < 40; pass++) {
        int best = -1;
        for (int k = 0; k < 4096; k++) if (callhist[k].n && (best < 0 || callhist[k].n > callhist[best].n)) best = k;
        if (best < 0) break;
        LOG("[callhist] %6u  f_%08x\n", callhist[best].n, callhist[best].addr);
        callhist[best].n = 0;
    }
}

void rt_call(EE *c, uint32_t addr) {
    if (UNLIKELY(g_callhist_start >= 0) && (long long)g_vsyncs >= g_callhist_start && (long long)g_vsyncs < g_callhist_start + 120) {
        uint32_t h = (addr >> 2) & 4095;
        for (int k = 0; k < 4096; k++) {
            uint32_t i = (h + k) & 4095;
            if (callhist[i].n == 0) { callhist[i].addr = addr; callhist[i].n = 1; break; }
            if (callhist[i].addr == addr) { callhist[i].n++; break; }
        }
    }
    if (UNLIKELY(g_calltrace_ra) && RW(31) == g_calltrace_ra) {
        static int n;
        if (n++ < 10) LOG("[calltrace] %08x -> %08x (a0=%08x)\n", g_calltrace_ra, addr, RW(4));
    }
    GuestFn fn = rt_lookup(addr);
    if (UNLIKELY(!fn)) {
        LOG("[dispatch] call to unknown address %08x (ra=%08x)\n", addr, RW(31));
        rt_fatal(c, "indirect call to unknown code");
    }
    fn(c);
}

/* ---- diagnostics -------------------------------------------------------- */
void rt_dump(EE *c) {
    static const char *n[32] = {"zr","at","v0","v1","a0","a1","a2","a3","t0","t1","t2","t3","t4","t5","t6","t7",
                                "s0","s1","s2","s3","s4","s5","s6","s7","t8","t9","k0","k1","gp","sp","fp","ra"};
    LOG("  thread %u, last pc %08x\n", c->thread, c->pc);
    for (int k = 0; k < 32; k += 4)
        LOG("  %s=%08x %s=%08x %s=%08x %s=%08x\n", n[k], RW(k), n[k+1], RW(k+1), n[k+2], RW(k+2), n[k+3], RW(k+3));
    void *bt[64];
    int nb = backtrace(bt, 64);
    backtrace_symbols_fd(bt, nb, 2);
}

void rt_fatal(EE *c, const char *why) {
    LOG("[fatal] %s\n", why);
    if (c) rt_dump(c);
    fflush(stderr);
    _exit(3);
}

void rt_unimpl(EE *c, uint32_t pc, const char *op) {
    LOG("[unimpl] %s at %08x (%s)\n", op, pc, rt_func_name(pc));
    rt_fatal(c, "unimplemented instruction");
}

void rt_bad_jump(EE *c, uint32_t pc, uint32_t target) {
    LOG("[jump] bad jump at %08x to %08x\n", pc, target);
    rt_fatal(c, "bad jump");
}

void rt_ra_mismatch(EE *c, uint32_t pc, uint32_t expect) {
    static int n;
    if (n++ < 20) LOG("[ra] return at %08x to %08x, entered with ra %08x\n", pc, RW(31), expect);
}

void rt_break(EE *c, uint32_t pc, uint32_t code) {
    static int n;
    if (n++ < 20) LOG("[break] code %u at %08x (%s)\n", code, pc, rt_func_name(pc));
}

void rt_trap(EE *c, uint32_t pc, const char *what) {
    static int n;
    if (n++ < 20) LOG("[trap] %s at %08x (%s)\n", what, pc, rt_func_name(pc));
}

void kern_dump_threads(void);
volatile int g_usr1_req;
static void on_usr1(int sig) { (void)sig; g_usr1_req = 1; }   /* the work runs on the vblank thread (LOG is not async-signal-safe) */
void rt_usr1_work(void) {
    kern_dump_threads();
    rt_callhist_dump();
    const char *vd = getenv("HP_VRAMDUMP");
    if (vd) {
        extern uint8_t *g_vram;
        FILE *f = fopen(vd, "wb");
        if (f) { fwrite(g_vram, 1, 4u << 20, f); fclose(f); LOG("[gs] VRAM dumped to %s\n", vd); }
    }
    const char *rd = getenv("HP_RAMDUMP");
    if (rd) { FILE *f = fopen(rd, "wb"); if (f) { fwrite(g_ram, 1, RAM_SIZE, f); fclose(f); LOG("[mem] RAM dumped to %s\n", rd); } }
    const char *peek = getenv("HP_PEEK");   /* "addr:len,addr:len" (hex) */
    while (peek && *peek) {
        char *end;
        uint32_t a = (uint32_t)strtoul(peek, &end, 16), n = 64;
        if (*end == ':') n = (uint32_t)strtoul(end + 1, &end, 16);
        for (uint32_t k = 0; k < n; k += 16)
            LOG("[peek] %08x: %08x %08x %08x %08x\n", a + k, ld32(a + k), ld32(a + k + 4), ld32(a + k + 8), ld32(a + k + 12));
        peek = *end == ',' ? end + 1 : NULL;
    }
}

static void on_signal(int sig) {
    LOG("[signal] %d\n", sig);
    EE *c = kern_current_ctx();
    rt_fatal(c, "host signal");
}

/* ---- entry -------------------------------------------------------------- */
static void load_image(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { LOG("cannot open %s\n", path); exit(1); }
    size_t n = fread(g_ram + IMAGE_BASE, 1, RAM_SIZE - IMAGE_BASE, f);
    fclose(f);
    LOG("[load] %zu bytes at %08x, entry %08x, %u functions\n", n, IMAGE_BASE, g_entry_point, g_func_count);
}

#include "gs.h"
static void *guest_main(void *arg) { (void)arg; kern_run_main(g_entry_point); return NULL; }

int main(int argc, char **argv) {
    const char *image = argc > 1 ? argv[1] : "build/recomp/image.bin";
    if (argc > 2) snprintf(g_disc_dir, sizeof g_disc_dir, "%s", argv[2]);
    g_trace = getenv("HP_TRACE") ? atoi(getenv("HP_TRACE")) : 0;
    if (getenv("HP_WATCH")) g_watch = (uint32_t)strtoul(getenv("HP_WATCH"), NULL, 16) & 0x1ffffff0u;
    g_callhist_start = getenv("HP_CALLHIST") ? atol(getenv("HP_CALLHIST")) : -1;
    g_calltrace_ra = getenv("HP_CALLTRACE") ? (uint32_t)strtoul(getenv("HP_CALLTRACE"), NULL, 16) : 0;
    signal(SIGSEGV, on_signal);
    signal(SIGBUS, on_signal);
    signal(SIGILL, on_signal);
    signal(SIGFPE, on_signal);
    signal(SIGUSR1, on_usr1);
    mem_init();
    dispatch_init();
    load_image(image);
    hw_init();
    gs_init();
    if (getenv("HP_HEADLESS")) kern_run_main(g_entry_point);
    /* guest code runs on its own thread; the main thread owns the window */
    pthread_t gt;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 64u << 20);
    pthread_create(&gt, &a, guest_main, NULL);
    display_loop();
    return 0;
}
