/* High-level EE kernel: threads, semaphores, interrupt handlers, syscalls.
 *
 * Every guest thread runs on its own host thread, but only one of them runs
 * guest code at a time (the "baton", `cur`).  Scheduling follows the PS2
 * kernel: strict priority (0 = highest), FIFO inside a priority, no time
 * slicing.  Interrupts are latched by hardware code from any host thread and
 * delivered at safe points: syscalls and POLL() on backward branches.
 */
#include "rt.h"
#include "ee_gen.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { TS_RUN = 1, TS_READY = 2, TS_WAIT = 4, TS_SUSPEND = 8, TS_WAITSUSPEND = 12, TS_DORMANT = 16 };
enum { WT_NONE = 0, WT_SLEEP = 1, WT_SEMA = 2 };

#define MAX_THREADS 256
#define MAX_SEMAS 512
#define MAX_HANDLERS 64
#define IRQ_STACK_TOP 0x000FFF00u
#define KERN_SCRATCH  0x00080000u   /* free RAM below the executable for kernel-owned data */
#define KSYSCALL_TAB  0x80001000u   /* syscall table in kernel RAM: the SDK searches and patches it */
#define KFAKE_FN(n)   (0x80002000u + 16u * (uint32_t)(n))

typedef struct {
    int used, status, prio, init_prio, wait_type, wait_id, wakeups, kill, host_started;
    uint32_t entry, stack, stack_size, gp, attr, option, arg;
    uint64_t seq;
    EE *c;
    pthread_t th;
    pthread_cond_t cv;
} KThread;

typedef struct { int used, count, max, init, waiters; uint32_t attr, option; } KSema;
typedef struct { int used, cause, enabled; uint32_t fn, arg, gp; } KHandler;

static pthread_mutex_t km = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t idle_cv = PTHREAD_COND_INITIALIZER;
static KThread th[MAX_THREADS];
static KSema sem[MAX_SEMAS];
static KHandler intc_h[MAX_HANDLERS], dmac_h[MAX_HANDLERS];
static int cur = -1;
static uint64_t seq_ctr;
static __thread int my_tid = -1;
static int in_irq, need_resched, irq_enabled = 1;
static uint32_t intc_stat, intc_mask, dmac_cis, dmac_cim;
static uint64_t irq_seq, intc_seq[16], dmac_seq[16];   /* arrival order of pending interrupts */
static uint32_t heap_start, heap_end, main_stack, main_stack_size;
static uint32_t vsync_flag_addr, vsync_data_addr;
static uint32_t gs_imr = 0x7f00;
static EE irqc;

/* Alarms (SDK alarm patch, syscalls 252..255): one-shot callbacks after N hsyncs. */
typedef struct { int used, fired; uint64_t due; uint32_t fn, arg, gp, time; } KAlarm;
#define MAX_ALARMS 64
static KAlarm alarms[MAX_ALARMS];
static pthread_cond_t alarm_cv = PTHREAD_COND_INITIALIZER;
static uint64_t host_ns(void);
#define HSYNC_NS 63556ull

/* ---- scheduling (km held) ---------------------------------------------- */
static int pick(void) {
    int best = -1;
    for (int t = 1; t < MAX_THREADS; t++) {
        KThread *k = &th[t];
        if (!k->used || (k->status != TS_RUN && k->status != TS_READY)) continue;
        if (best < 0 || k->prio < th[best].prio || (k->prio == th[best].prio && k->seq < th[best].seq)) best = t;
    }
    return best;
}

static void *thread_entry(void *arg);

static void wake_host(int t) {
    if (!th[t].host_started) {
        th[t].host_started = 1;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 16u << 20);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        pthread_create(&th[t].th, &a, thread_entry, (void *)(intptr_t)t);
        pthread_attr_destroy(&a);
    } else {
        pthread_cond_signal(&th[t].cv);
    }
}

static void wait_turn(int t) {
    while (cur != t && !th[t].kill) pthread_cond_wait(&th[t].cv, &km);
    if (th[t].kill) {
        th[t].kill = 0;
        th[t].host_started = 0;
        pthread_mutex_unlock(&km);
        pthread_exit(NULL);
    }
}

static void deliver_locked(void);
void iop_sif0_done(void);

/* Give the CPU to the best runnable thread; returns once this thread runs again
 * (or never, when `exiting`). */
static void schedule_ex(int exiting) {
    for (;;) {
        int best = pick();
        if (best >= 0) {
            if (best == my_tid && !exiting) { th[best].status = TS_RUN; cur = best; return; }
            if (!exiting && my_tid > 0 && th[my_tid].status == TS_RUN) th[my_tid].status = TS_READY;
            th[best].status = TS_RUN;
            cur = best;
            wake_host(best);
            if (exiting) return;
            wait_turn(my_tid);
            return;
        }
        /* Nothing runnable: this host thread idles and runs interrupt handlers. */
        cur = -1;
        while (!g_irq_pending) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 50 * 1000000;
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&idle_cv, &km, &ts);
        }
        deliver_locked();
    }
}

static void schedule(void) {
    if (in_irq) { need_resched = 1; return; }
    schedule_ex(0);
}

static void ready(int t) {
    th[t].status = TS_READY;
    th[t].wait_type = WT_NONE;
    th[t].seq = ++seq_ctr;
}

/* ---- interrupts ---------------------------------------------------------- */
static void call_handler(const KHandler *h, uint32_t a0) {
    GuestFn fn = rt_lookup(h->fn);
    if (!fn) { LOG("[intr] handler %08x is not code\n", h->fn); return; }
    EE *c = &irqc;
    c->r[4].d[0] = a0; c->r[5].d[0] = h->arg; c->r[6].d[0] = 0;
    c->r[28].d[0] = h->gp; c->r[29].d[0] = IRQ_STACK_TOP; c->r[31].d[0] = 0;
    c->thread = 0;
    pthread_mutex_unlock(&km);
    fn(c);
    pthread_mutex_lock(&km);
}

static void deliver_locked(void) {
    if (in_irq) return;
    in_irq = 1;
    for (;;) {
        g_irq_pending = 0;
        uint32_t p = intc_stat & intc_mask, d = dmac_cis & dmac_cim;
        int al = -1;
        for (int k = 0; k < MAX_ALARMS; k++) if (alarms[k].used && alarms[k].fired) { al = k; break; }
        if (al >= 0) {
            KAlarm a = alarms[al];
            alarms[al].used = 0;
            KHandler h = {1, 0, 1, a.fn, 0, a.gp};
            GuestFn fn = rt_lookup(a.fn);
            if (fn) {
                EE *c = &irqc;
                c->r[4].d[0] = (uint32_t)al; c->r[5].d[0] = a.time; c->r[6].d[0] = a.arg;
                c->r[28].d[0] = a.gp; c->r[29].d[0] = IRQ_STACK_TOP; c->r[31].d[0] = 0;
                pthread_mutex_unlock(&km);
                fn(c);
                pthread_mutex_lock(&km);
            }
            (void)h;
            continue;
        }
        if (!p && !d) break;
        /* deliver the oldest pending interrupt first, like the hardware would */
        int line = -1, ch = -1;
        uint64_t best = ~0ull;
        for (uint32_t m = p; m; m &= m - 1) { int k = __builtin_ctz(m); if (intc_seq[k] < best) { best = intc_seq[k]; line = k; } }
        for (uint32_t m = d; m; m &= m - 1) { int k = __builtin_ctz(m); if (dmac_seq[k] < best) { best = dmac_seq[k]; line = -1; ch = k; } }
        if (line >= 0) {
            intc_stat &= ~(1u << line);
            for (int k = 0; k < MAX_HANDLERS; k++)
                if (intc_h[k].used && intc_h[k].enabled && intc_h[k].cause == line) call_handler(&intc_h[k], line);
            if (line == 2 && vsync_flag_addr) { st32(vsync_flag_addr, 1); if (vsync_data_addr) st64(vsync_data_addr, 0); }
        } else {
            dmac_cis &= ~(1u << ch);
            if (g_trace > 1 && ch == 5) LOG("[kern] dmac5 interrupt -> handlers\n");
            if (g_trace && (ch == 3 || ch == 4)) LOG("[kern] dmac%d interrupt -> handlers\n", ch);
            for (int k = 0; k < MAX_HANDLERS; k++)
                if (dmac_h[k].used && dmac_h[k].enabled && dmac_h[k].cause == ch) call_handler(&dmac_h[k], ch);
            if (ch == 5) {   /* SIF0 packet consumed: let the IOP send the next one */
                pthread_mutex_unlock(&km);
                iop_sif0_done();
                pthread_mutex_lock(&km);
            }
        }
    }
    in_irq = 0;
}

void rt_irq_poll(EE *c) {
    if (!irq_enabled || my_tid < 0) return;
    pthread_mutex_lock(&km);
    if (!in_irq && cur == my_tid) {
        deliver_locked();
        if (need_resched) { need_resched = 0; schedule_ex(0); }
    }
    pthread_mutex_unlock(&km);
}

void kern_raise_intc(int line) {
    pthread_mutex_lock(&km);
    if (!(intc_stat & (1u << line))) intc_seq[line] = ++irq_seq;
    intc_stat |= 1u << line;
    if (intc_stat & intc_mask) { g_irq_pending = 1; pthread_cond_signal(&idle_cv); }
    pthread_mutex_unlock(&km);
}

void kern_raise_dmac(int ch) {
    pthread_mutex_lock(&km);
    if (!(dmac_cis & (1u << ch))) dmac_seq[ch] = ++irq_seq;
    dmac_cis |= 1u << ch;
    if (dmac_cis & dmac_cim) { g_irq_pending = 1; pthread_cond_signal(&idle_cv); }
    pthread_mutex_unlock(&km);
}

uint64_t kern_gs_imr(void) { return gs_imr; }
uint32_t kern_intc_stat(void) { return intc_stat; }
void kern_intc_ack(uint32_t bits) { pthread_mutex_lock(&km); intc_stat &= ~bits; pthread_mutex_unlock(&km); }
uint32_t kern_intc_mask(void) { return intc_mask; }
void kern_intc_toggle_mask(uint32_t bits) {
    pthread_mutex_lock(&km);
    intc_mask ^= bits;
    if (intc_stat & intc_mask) g_irq_pending = 1;
    pthread_mutex_unlock(&km);
}
uint32_t kern_dmac_stat(void) { return dmac_cis | (dmac_cim << 16); }
void kern_dmac_write_stat(uint32_t v) {
    pthread_mutex_lock(&km);
    dmac_cis &= ~(v & 0x3ff);
    dmac_cim ^= (v >> 16) & 0x3ff;
    if (dmac_cis & dmac_cim) g_irq_pending = 1;
    pthread_mutex_unlock(&km);
}

void rt_ei(EE *c) { irq_enabled = 1; if (g_irq_pending) rt_irq_poll(c); }
void rt_di(EE *c) { irq_enabled = 0; }
void rt_eret(EE *c, uint32_t pc) { LOG("[kern] eret at %08x\n", pc); }

/* ---- COP0 --------------------------------------------------------------- */
static uint64_t host_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
static uint64_t t0_ns;
static uint32_t count_bias;
/* Emulated time: wall time minus the time spent emulating GS/VU work, so the
 * game's timers see rendering as (nearly) free, as on the real hardware. */
static uint64_t g_stolen_ns, g_freeze_at;
static int g_freeze_depth;
static pthread_mutex_t vm = PTHREAD_MUTEX_INITIALIZER;
uint64_t virt_ns(void) {
    pthread_mutex_lock(&vm);
    uint64_t v = (g_freeze_depth ? g_freeze_at : host_ns()) - g_stolen_ns;
    pthread_mutex_unlock(&vm);
    return v;
}
/* While frozen the emulated clock stands still (monotonic, never steps back). */
void kern_freeze_begin(void) {
    pthread_mutex_lock(&vm);
    if (g_freeze_depth++ == 0) g_freeze_at = host_ns();
    pthread_mutex_unlock(&vm);
}
void kern_freeze_end(void) {
    pthread_mutex_lock(&vm);
    if (--g_freeze_depth == 0) g_stolen_ns += host_ns() - g_freeze_at;
    pthread_mutex_unlock(&vm);
}

uint32_t rt_mfc0(EE *c, int reg, int sel) {
    switch (reg) {
    case 9: return (uint32_t)((virt_ns() - t0_ns) * 294912 / 1000000) + count_bias;   /* 294.912 MHz */
    case 12: return 0x70020c11u | (irq_enabled ? 0x10000 : 0);   /* CU0-2, EDI, IM2/3, user mode */
    case 15: return 0x2e20;
    case 16: return 0x440;
    case 25: return (uint32_t)((virt_ns() - t0_ns) * 294912 / 1000000);
    default: return 0;
    }
}
void rt_mtc0(EE *c, int reg, uint32_t v) {
    if (reg == 9) count_bias = v - rt_mfc0(c, 9, 0) + count_bias;
    else if (reg == 12) irq_enabled = (v >> 16) & 1;
}
int rt_cpcond0(EE *c) { return 1; }

/* ---- threads ------------------------------------------------------------ */
static void *thread_entry(void *arg) {
    int t = (int)(intptr_t)arg;
    pthread_mutex_lock(&km);
    my_tid = t;
    wait_turn(t);
    KThread *k = &th[t];
    EE *c = k->c;
    memset(c, 0, sizeof *c);
    c->vf[0].f[3] = 1.0f;
    c->thread = t;
    R(4) = k->arg;
    R(28) = k->gp;
    R(29) = (k->stack + k->stack_size - 0x40) & ~15u;
    R(31) = 0;
    uint32_t entry = k->entry;
    pthread_mutex_unlock(&km);
    GuestFn fn = rt_lookup(entry);
    if (!fn) rt_fatal(c, "thread entry is not code");
    fn(c);
    /* returning from the entry function is ExitThread */
    pthread_mutex_lock(&km);
    k->status = TS_DORMANT;
    k->host_started = 0;
    schedule_ex(1);
    pthread_mutex_unlock(&km);
    return NULL;
}

static int new_thread(void) {
    for (int t = 1; t < MAX_THREADS; t++)
        if (!th[t].used) {
            memset(&th[t], 0, sizeof th[t]);
            th[t].used = 1;
            th[t].c = calloc(1, sizeof(EE));
            pthread_cond_init(&th[t].cv, NULL);
            return t;
        }
    return -1;
}

void kern_dump_threads(void) {
    for (int t = 1; t < MAX_THREADS; t++) {
        EE *c = th[t].c;
        if (!c) continue;
        LOG("[thread %d] used %d status %d prio %d wait %d/%d pc %08x (%s) ra %08x sp %08x%s\n", t, th[t].used, th[t].status, th[t].prio,
            th[t].wait_type, th[t].wait_id, c->pc, rt_func_name(c->pc), RW(31), RW(29), t == cur ? " RUNNING" : "");
        if (!th[t].used) continue;
        if (getenv("HP_PCRING")) {
            char last[96] = "";
            char line[4096]; int n = 0;
            uint32_t end = c->pcidx;   /* snapshot: the thread may still be running */
            for (uint32_t k = end > 4096 ? end - 4096 : 0; k < end; k++) {
                const char *nm = rt_func_name(c->pcring[k & 4095]);
                char fn[96]; snprintf(fn, sizeof fn, "%s", nm); char *pl = strchr(fn, '+'); if (pl) *pl = 0;
                if (!strcmp(fn, last)) continue;
                snprintf(last, sizeof last, "%s", fn);
                n += snprintf(line + n, sizeof line - n, " %s", nm);
                if (n > 3900) { LOG("[ring %d]%s\n", t, line); n = 0; }
            }
            if (n) LOG("[ring %d]%s\n", t, line);
        }
    }
}

EE *kern_current_ctx(void) {
    if (in_irq) return &irqc;
    return my_tid > 0 ? th[my_tid].c : NULL;
}

/* ---- syscalls ----------------------------------------------------------- */
#define A0 RW(4)
#define A1 RW(5)
#define A2 RW(6)
#define A3 RW(7)
#define T0 RW(8)
#define RET(v) (R(2) = SX((uint32_t)(v)))

static int add_handler(KHandler *tab, int cause, uint32_t fn, uint32_t arg, uint32_t gp) {
    for (int k = 1; k < MAX_HANDLERS; k++)
        if (!tab[k].used) {
            tab[k] = (KHandler){1, cause, 1, fn, arg, gp};
            return k;
        }
    return -1;
}

static int sys_thread(EE *c, int n) {
    switch (n) {
    case 32: {  /* CreateThread(ee_thread_t *) */
        int t = new_thread();
        if (t < 0) return -1;
        KThread *k = &th[t];
        k->entry = g32(A0 + 4); k->stack = g32(A0 + 8); k->stack_size = g32(A0 + 12);
        k->gp = g32(A0 + 16); k->init_prio = k->prio = g32(A0 + 20);
        k->attr = g32(A0 + 28); k->option = g32(A0 + 32);
        k->status = TS_DORMANT;
        if (g_trace) LOG("[kern] CreateThread %d entry %08x stack %08x+%x prio %d\n", t, k->entry, k->stack, k->stack_size, k->prio);
        return t;
    }
    case 33: if (A0 < MAX_THREADS && th[A0].used && th[A0].status == TS_DORMANT) { th[A0].used = 0; return A0; } return -1;
    case 34: {  /* StartThread(id, arg) */
        int t = A0;
        if (t <= 0 || t >= MAX_THREADS || !th[t].used || th[t].status != TS_DORMANT) return -1;
        th[t].arg = A1;
        th[t].prio = th[t].init_prio;
        ready(t);
        if (g_trace) LOG("[kern] StartThread %d arg %08x\n", t, A1);
        schedule();
        return t;
    }
    case 35: case 36: {  /* ExitThread / ExitDeleteThread */
        th[my_tid].status = TS_DORMANT;
        if (n == 36) th[my_tid].used = 0;
        th[my_tid].host_started = 0;
        schedule_ex(1);
        pthread_mutex_unlock(&km);
        pthread_exit(NULL);
    }
    case 37: case 38: {  /* TerminateThread */
        int t = A0;
        if (t <= 0 || t >= MAX_THREADS || !th[t].used || t == my_tid) return -1;
        if (th[t].status & (TS_READY | TS_WAIT | TS_SUSPEND)) {
            th[t].status = TS_DORMANT;
            if (th[t].host_started) { th[t].kill = 1; pthread_cond_signal(&th[t].cv); }
        }
        return t;
    }
    case 41: case 42: {  /* ChangeThreadPriority */
        int t = A0 ? (int)A0 : my_tid;
        if (t <= 0 || t >= MAX_THREADS || !th[t].used) return -1;
        int old = th[t].prio;
        th[t].prio = A1;
        if (th[t].status == TS_READY) th[t].seq = ++seq_ctr;
        schedule();
        return old;
    }
    case 43: case 44: {  /* RotateThreadReadyQueue(prio) */
        if (my_tid > 0 && th[my_tid].prio == (int)A0) th[my_tid].seq = ++seq_ctr;
        else
            for (int t = 1; t < MAX_THREADS; t++)
                if (th[t].used && th[t].status == TS_READY && th[t].prio == (int)A0) { th[t].seq = ++seq_ctr; break; }
        schedule();
        return 0;
    }
    case 45: case 46: {  /* ReleaseWaitThread */
        int t = A0;
        if (t > 0 && t < MAX_THREADS && th[t].used && th[t].status == TS_WAIT) {
            if (th[t].wait_type == WT_SEMA) sem[th[t].wait_id].waiters--;
            ready(t); schedule();
        }
        return t;
    }
    case 47: return in_irq ? cur : my_tid;
    case 48: case 49: {  /* ReferThreadStatus(id, status *) */
        int t = A0 ? (int)A0 : my_tid;
        if (t <= 0 || t >= MAX_THREADS || !th[t].used) return -1;
        KThread *k = &th[t];
        if (A1) {
            uint32_t p = A1;
            s32(p + 0, k->status); s32(p + 4, k->entry); s32(p + 8, k->stack); s32(p + 12, k->stack_size);
            s32(p + 16, k->gp); s32(p + 20, k->init_prio); s32(p + 24, k->prio); s32(p + 28, k->attr);
            s32(p + 32, k->option); s32(p + 36, k->wait_type); s32(p + 40, k->wait_id); s32(p + 44, k->wakeups);
        }
        return k->status;
    }
    case 50: {  /* SleepThread */
        KThread *k = &th[my_tid];
        if (k->wakeups > 0) { k->wakeups--; return 0; }
        k->status = TS_WAIT; k->wait_type = WT_SLEEP;
        schedule();
        return 0;
    }
    case 51: case 52: {  /* WakeupThread */
        int t = A0;
        if (t <= 0 || t >= MAX_THREADS || !th[t].used) return -1;
        if (th[t].status == TS_WAIT && th[t].wait_type == WT_SLEEP) { ready(t); schedule(); }
        else th[t].wakeups++;
        return t;
    }
    case 53: case 54: {  /* CancelWakeupThread */
        int t = A0 ? (int)A0 : my_tid;
        int w = th[t].wakeups; th[t].wakeups = 0; return w;
    }
    case 55: case 56: {  /* SuspendThread */
        int t = A0;
        if (t > 0 && t < MAX_THREADS && th[t].used) {
            if (th[t].status == TS_READY) th[t].status = TS_SUSPEND;
            else if (th[t].status == TS_WAIT) th[t].status = TS_WAITSUSPEND;
        }
        return t;
    }
    case 57: case 58: {  /* ResumeThread */
        int t = A0;
        if (t > 0 && t < MAX_THREADS && th[t].used) {
            if (th[t].status == TS_SUSPEND) { ready(t); schedule(); }
            else if (th[t].status == TS_WAITSUSPEND) th[t].status = TS_WAIT;
        }
        return t;
    }
    }
    return -1;
}

static int sys_sema(EE *c, int n) {
    switch (n) {
    case 64: {  /* CreateSema(ee_sema_t *) */
        for (int s = 1; s < MAX_SEMAS; s++)
            if (!sem[s].used) {
                sem[s] = (KSema){1, (int)g32(A0 + 8), (int)g32(A0 + 4), (int)g32(A0 + 8), 0, g32(A0 + 16), g32(A0 + 20)};
                if (g_trace) LOG("[kern] CreateSema %d init %d max %d\n", s, sem[s].count, sem[s].max);
                return s;
            }
        return -1;
    }
    case 65: {  /* DeleteSema */
        int s = A0;
        if (s <= 0 || s >= MAX_SEMAS || !sem[s].used) return -1;
        for (int t = 1; t < MAX_THREADS; t++)
            if (th[t].used && th[t].status == TS_WAIT && th[t].wait_type == WT_SEMA && th[t].wait_id == s) ready(t);
        sem[s].used = 0;
        schedule();
        return s;
    }
    case 66: case 67: {  /* SignalSema / iSignalSema */
        int s = A0;
        if (s <= 0 || s >= MAX_SEMAS || !sem[s].used) return -1;
        int w = -1;
        for (int t = 1; t < MAX_THREADS; t++)
            if (th[t].used && th[t].status == TS_WAIT && th[t].wait_type == WT_SEMA && th[t].wait_id == s &&
                (w < 0 || th[t].seq < th[w].seq)) w = t;
        if (w > 0) { sem[s].waiters--; ready(w); schedule(); }
        else if (sem[s].count < sem[s].max || sem[s].max <= 0) sem[s].count++;
        return s;
    }
    case 68: {  /* WaitSema */
        int s = A0;
        if (s <= 0 || s >= MAX_SEMAS || !sem[s].used) return -1;
        if (sem[s].count > 0) { sem[s].count--; return s; }
        if (in_irq) return -1;
        KThread *k = &th[my_tid];
        k->status = TS_WAIT; k->wait_type = WT_SEMA; k->wait_id = s; k->seq = ++seq_ctr;
        sem[s].waiters++;
        schedule();
        return sem[s].used ? s : -1;
    }
    case 69: case 70: {  /* PollSema */
        int s = A0;
        if (s <= 0 || s >= MAX_SEMAS || !sem[s].used) return -1;
        if (sem[s].count > 0) { sem[s].count--; return s; }
        return -419;
    }
    case 71: case 72: {  /* ReferSemaStatus */
        int s = A0;
        if (s <= 0 || s >= MAX_SEMAS || !sem[s].used) return -1;
        s32(A1, sem[s].count); s32(A1 + 4, sem[s].max); s32(A1 + 8, sem[s].init);
        s32(A1 + 12, sem[s].waiters); s32(A1 + 16, sem[s].attr); s32(A1 + 20, sem[s].option);
        return s;
    }
    }
    return -1;
}

int64_t hw_sif_get_reg(uint32_t reg);
void hw_sif_set_reg(uint32_t reg, uint32_t v);
void hw_set_crt(int interlace, int mode, int field);
void iop_sif_dma(uint32_t list, int count);

static void unknown_syscall(EE *c, int n) {
    static int seen[512];
    int k = n & 511;
    if (!seen[k]++)
        LOG("[kern] unhandled syscall %d (a0=%08x a1=%08x a2=%08x a3=%08x) from %s\n", n, A0, A1, A2, A3,
            rt_func_name(c->pc));
}

void rt_syscall(EE *c, uint32_t pc) {
    int n = (int)RSW(3);
    c->pc = pc;
    if (g_trace > 1) LOG("[sys] %d a0=%08x a1=%08x a2=%08x from %s t%d ra %08x\n", n, A0, A1, A2, rt_func_name(pc), my_tid, RW(31));
    int an = n < 0 ? -n : n;
    if (an < 256) {
        /* handlers installed by the game (SetSyscall or a direct table patch) run as guest code */
        uint32_t h = ld32(KSYSCALL_TAB + 4u * (uint32_t)an);
        if (h != KFAKE_FN(an)) {
            GuestFn fn = rt_lookup(h);
            if (fn) { fn(c); return; }
        }
    }
    pthread_mutex_lock(&km);
    if (!in_irq && cur == my_tid && g_irq_pending) {
        deliver_locked();
        if (need_resched) { need_resched = 0; schedule_ex(0); }
    }
    int64_t r = 0;
    switch (an) {
    case 2: hw_set_crt(A0, A1, A2); break;
    case 4: LOG("[kern] Exit(%d)\n", (int)A0); fflush(stderr); _exit((int)A0);
    case 6: case 7: LOG("[kern] LoadExecPS2(%s)\n", gstr(A0)); fflush(stderr); _exit(0);
    case 16: r = add_handler(intc_h, A0, A1, A3, RW(28)); if (g_trace) LOG("[kern] AddIntcHandler(%d, %08x) = %d\n", A0, A1, (int)r); break;
    case 17: if (A1 < MAX_HANDLERS) intc_h[A1].used = 0; break;
    case 18: r = add_handler(dmac_h, A0, A1, A3, RW(28)); if (g_trace) LOG("[kern] AddDmacHandler(%d, %08x) = %d\n", A0, A1, (int)r); break;
    case 19: if (A1 < MAX_HANDLERS) dmac_h[A1].used = 0; if (g_trace) LOG("[kern] RemoveDmacHandler(%d, %d)\n", A0, A1); break;
    case 20: case 26: r = !(intc_mask >> A0 & 1); intc_mask |= 1u << A0; if (intc_stat & intc_mask) g_irq_pending = 1; break;
    case 21: case 27: r = intc_mask >> A0 & 1; intc_mask &= ~(1u << A0); break;
    case 22: case 28: r = !(dmac_cim >> A0 & 1); dmac_cim |= 1u << A0; if (dmac_cis & dmac_cim) g_irq_pending = 1;
        if (g_trace && A0 != 1 && A0 != 5) LOG("[kern] EnableDmac(%d) cis %03x cim %03x\n", A0, dmac_cis, dmac_cim); break;
    case 23: case 29: r = dmac_cim >> A0 & 1; dmac_cim &= ~(1u << A0);
        if (g_trace && A0 != 1 && A0 != 5) LOG("[kern] DisableDmac(%d) cis %03x cim %03x\n", A0, dmac_cis, dmac_cim); break;
    case 32: case 33: case 34: case 35: case 36: case 37: case 38: case 41: case 42: case 43: case 44:
    case 45: case 46: case 47: case 48: case 49: case 50: case 51: case 52: case 53: case 54: case 55:
    case 56: case 57: case 58:
        r = sys_thread(c, an); break;
    case 60: {  /* SetupThread(gp, stack, stack_size, args, root) */
        uint32_t stack = A1, size = A2, args = A3;
        if (stack == 0xffffffffu) stack = RAM_SIZE - size;
        main_stack = stack; main_stack_size = size;
        th[my_tid].gp = A0; th[my_tid].stack = stack; th[my_tid].stack_size = size;
        if (args) {   /* argc/argv for main(): one argument, the boot path */
            static const char boot[] = "cdrom0:\\SLUS_209.26;1";
            s32(args, 1);
            s32(args + 4, args + 0x40);
            memcpy(gptr(args + 0x40), boot, sizeof boot);
        }
        r = stack + size;
        if (g_trace) LOG("[kern] SetupThread gp %08x stack %08x size %x -> sp %08x\n", A0, stack, size, (uint32_t)r);
        break;
    }
    case 61:  /* SetupHeap(start, size) */
        heap_start = A0;
        heap_end = A1 == 0xffffffffu ? main_stack : A0 + A1;
        r = heap_end;
        if (g_trace) LOG("[kern] SetupHeap %08x..%08x\n", heap_start, heap_end);
        break;
    case 62: r = heap_end; break;
    case 64: case 65: case 66: case 67: case 68: case 69: case 70: case 71: case 72:
        r = sys_sema(c, an); break;
    case 252: case 253: {  /* SetAlarm / iSetAlarm(time, callback, arg) */
        r = -1;
        for (int k = 0; k < MAX_ALARMS; k++)
            if (!alarms[k].used) {
                alarms[k] = (KAlarm){1, 0, virt_ns() + (uint64_t)(A0 & 0xffff) * HSYNC_NS, A1, A2, RW(28), A0 & 0xffff};
                pthread_cond_signal(&alarm_cv);
                r = k;
                break;
            }
        break;
    }
    case 254: case 255:    /* ReleaseAlarm / iReleaseAlarm(id) */
        r = -1;
        if (A0 < MAX_ALARMS && alarms[A0].used) { alarms[A0].used = 0; r = A0; }
        break;
    case 74: break;   /* SetOsdConfigParam */
    case 75:          /* GetOsdConfigParam: English, 4:3, no spdif */
        s32(A0, 1u << 16);
        break;
    case 100: case 104: break;   /* FlushCache */
    case 107: break;             /* sceSifStopDma */
    case 112: r = gs_imr; break;
    case 113: r = gs_imr; gs_imr = A0; break;
    case 115: vsync_flag_addr = A0; vsync_data_addr = A1; break;
    case 116:  /* SetSyscall(num, fn) */
        if (A0 < 256) st32(KSYSCALL_TAB + 4 * A0, A1);
        if (g_trace) LOG("[kern] SetSyscall(%d, %08x)\n", A0, A1);
        break;
    case 117: LOG("[print] %s", gstr(A0)); break;
    case 118: r = -1; break;     /* sceSifDmaStat: always finished */
    case 119:                    /* sceSifSetDma(list, count) */
        pthread_mutex_unlock(&km);
        iop_sif_dma(A0, A1);
        pthread_mutex_lock(&km);
        r = 1;
        break;
    case 120: break;             /* sceSifSetDChain */
    case 121: hw_sif_set_reg(A0, A1); r = 1; break;
    case 122: r = hw_sif_get_reg(A0); break;
    case 124: r = -1; break;     /* Deci2Call: no debug host */
    case 125: r = 0; break;      /* PSMode */
    case 126: r = 0; break;      /* MachineType */
    case 127: r = RAM_SIZE; break;
    default: unknown_syscall(c, n); r = 0; break;
    }
    pthread_mutex_unlock(&km);
    RET(r);
}

/* ---- boot ---------------------------------------------------------------- */
/* Sleep until the emulated clock reaches `target` (it stands still while frozen). */
static void sleep_until_virt(uint64_t target) {
    for (;;) {
        uint64_t now = virt_ns();
        if (now >= target) return;
        uint64_t d = target - now;
        if (d > 2000000) d = 2000000;
        struct timespec ts = {0, (long)d};
        nanosleep(&ts, NULL);
    }
}

static void *clock_monitor(void *arg) {
    (void)arg;
    extern unsigned long long g_vsyncs;
    uint64_t lw = host_ns(), lv = virt_ns();
    unsigned long long ls = g_vsyncs;
    for (;;) {
        struct timespec ts = {2, 0};
        nanosleep(&ts, NULL);
        uint64_t w = host_ns(), v = virt_ns();
        extern double g_prof[8];
        LOG("[clock] wall %.2fs: virtual %.2fs, vsyncs %llu | dma1 %.2f sprite %.2f tri %.2f poolwait %.2f\n", (w - lw) / 1e9, (v - lv) / 1e9,
            g_vsyncs - ls, g_prof[0], g_prof[1], g_prof[2], g_prof[3]);
        for (int k = 0; k < 8; k++) g_prof[k] = 0;
        lw = w; lv = v; ls = g_vsyncs;
    }
    return NULL;
}

static void *vblank_thread(void *arg) {
    (void)arg;
    uint64_t next = virt_ns();
    const uint64_t period = 16683333;   /* 59.94 Hz of emulated time */
    for (;;) {
        next += period;
        uint64_t now = virt_ns();
        if (next + 4 * period < now) next = now;   /* never try to catch up on many frames */
        sleep_until_virt(next);
        hw_vblank(1);
        kern_raise_intc(2);
        sleep_until_virt(next + 1300000);
        hw_vblank(0);
        kern_raise_intc(3);
    }
    return NULL;
}

uint64_t hw_timers_next(void);
void hw_timers_fire(void);

/* Deferred host callbacks (hardware completion events), run by the alarm thread. */
typedef struct { int used; uint64_t due; void (*fn)(int); int arg; } KEvent;
#define MAX_EVENTS 64
static KEvent events[MAX_EVENTS];

void kern_after(uint64_t ns, void (*fn)(int), int arg) {
    pthread_mutex_lock(&km);
    int slot = -1;
    for (int k = 0; k < MAX_EVENTS; k++)
        if (!events[k].used) { slot = k; break; }
    if (slot >= 0) events[slot] = (KEvent){1, virt_ns() + ns, fn, arg};
    else LOG("[kern] event table full, dropping event\n");
    pthread_cond_signal(&alarm_cv);
    pthread_mutex_unlock(&km);
}

void kern_timers_changed(void) {
    pthread_mutex_lock(&km);
    pthread_cond_signal(&alarm_cv);
    pthread_mutex_unlock(&km);
}

static void *alarm_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&km);
    for (;;) {
        uint64_t now = virt_ns(), next = 0;
        int fired = 0;
        int best = -1;
        for (int k = 0; k < MAX_EVENTS; k++)
            if (events[k].used && events[k].due <= now && (best < 0 || events[k].due < events[best].due)) best = k;
        if (best >= 0) {
            KEvent e = events[best];
            events[best].used = 0;
            pthread_mutex_unlock(&km);
            e.fn(e.arg);
            pthread_mutex_lock(&km);
            continue;
        }
        for (int k = 0; k < MAX_EVENTS; k++)
            if (events[k].used && (!next || events[k].due < next)) next = events[k].due;
        uint64_t tn = hw_timers_next();
        if (tn && tn <= now) {
            pthread_mutex_unlock(&km);
            hw_timers_fire();
            pthread_mutex_lock(&km);
            continue;
        }
        if (tn && (!next || tn < next)) next = tn;
        for (int k = 0; k < MAX_ALARMS; k++) {
            KAlarm *a = &alarms[k];
            if (!a->used || a->fired) continue;
            if (a->due <= now) { a->fired = 1; fired = 1; }
            else if (!next || a->due < next) next = a->due;
        }
        if (fired) { g_irq_pending = 1; pthread_cond_signal(&idle_cv); }
        if (!next) { pthread_cond_wait(&alarm_cv, &km); continue; }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t wait = next - now;
        ts.tv_sec += (time_t)(wait / 1000000000ull);
        ts.tv_nsec += (long)(wait % 1000000000ull);
        if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&alarm_cv, &km, &ts);
    }
    return NULL;
}

/* HP_PROF=start_vsync,count: sample the running guest thread's pc every 200 us during
   `count` vsyncs and print the hottest functions (per thread). */
static void *prof_thread(void *arg) {
    (void)arg;
    extern unsigned long long g_vsyncs;
    const char *e = getenv("HP_PROF");
    long start = atol(e), dur = 300;
    const char *q = strchr(e, ','); if (q) dur = atol(q + 1);
    enum { NB = 8192 };
    static struct { uint32_t fn; int t; unsigned n; } h[NB];
    unsigned total = 0;
    while ((long)g_vsyncs < start) usleep(2000);
    while ((long)g_vsyncs < start + dur) {
        int t = cur;
        if (t > 0 && t < MAX_THREADS && th[t].c) {
            uint32_t pc = th[t].c->pc;
            /* function start = nearest table entry at or below pc */
            uint32_t lo = 0, hi = g_func_count;
            while (hi - lo > 1) { uint32_t m = (lo + hi) / 2; if (g_func_table[m].addr <= pc) lo = m; else hi = m; }
            uint32_t fn = g_func_table[lo].addr, k = ((fn >> 2) * 31u + (uint32_t)t) & (NB - 1);
            for (int i = 0; i < NB; i++, k = (k + 1) & (NB - 1)) {
                if (!h[k].n) { h[k].fn = fn; h[k].t = t; h[k].n = 1; break; }
                if (h[k].fn == fn && h[k].t == t) { h[k].n++; break; }
            }
            total++;
        }
        usleep(200);
    }
    LOG("[prof] %u samples, vsyncs %ld..%ld\n", total, start, start + dur);
    for (int pass = 0; pass < 60; pass++) {
        int best = -1;
        for (int k = 0; k < NB; k++) if (h[k].n && (best < 0 || h[k].n > h[best].n)) best = k;
        if (best < 0) break;
        LOG("[prof] %6u %5.1f%%  t%d f_%08x\n", h[best].n, 100.0 * h[best].n / (total ? total : 1), h[best].t, h[best].fn);
        h[best].n = 0;
    }
    return NULL;
}

void kern_run_main(uint32_t entry) {
    t0_ns = virt_ns();
    for (int n = 0; n < 256; n++) st32(KSYSCALL_TAB + 4u * (uint32_t)n, KFAKE_FN(n));
    irqc.vf[0].f[3] = 1.0f;
    int t = new_thread();
    KThread *k = &th[t];
    k->status = TS_RUN; k->prio = k->init_prio = 0; k->host_started = 1; k->entry = entry;
    k->seq = ++seq_ctr;
    cur = my_tid = t;
    EE *c = k->c;
    c->vf[0].f[3] = 1.0f;
    c->thread = t;
    R(4) = 0;                        /* no boot arguments: crt0 uses the SetupThread buffer */
    R(29) = RAM_SIZE - 0x100;        /* temporary until SetupThread */
    pthread_t vb, at;
    pthread_create(&vb, NULL, vblank_thread, NULL);
    pthread_create(&at, NULL, alarm_thread, NULL);
    if (getenv("HP_CLOCK")) { pthread_t cm; pthread_create(&cm, NULL, clock_monitor, NULL); }
    if (getenv("HP_PROF")) { pthread_t pt; pthread_create(&pt, NULL, prof_thread, NULL); }
    GuestFn fn = rt_lookup(entry);
    if (!fn) rt_fatal(c, "entry point is not code");
    fn(c);
    LOG("[kern] main thread returned\n");
    fflush(stderr);
    _exit(0);
}
