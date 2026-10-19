/* Per-thread CPU accounting for speed work: threads register under a name; HP_STATS prints usage. */
#include "rt.h"
#include <TargetConditionals.h>
#include <mach/mach.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAXT 64
static struct { char name[16]; mach_port_t port; double last; } tt[MAXT];
static int nt;
static pthread_mutex_t tm = PTHREAD_MUTEX_INITIALIZER;

void hp_thread_register(const char *name) {
    pthread_setname_np(name);
    /* the emulation threads are latency-critical: keep them on the performance cores (with the
       default QoS macOS moves them to the efficiency cores as soon as other work runs) */
    if (!getenv("HP_NOQOS")) {
        /* threads named in HP_QOSLOW (default: the GS thread, which only has to keep up) get a lower
           class, so they do not take a performance core from the EE */
        const char *low = getenv("HP_QOSLOW") ? getenv("HP_QOSLOW") : "gs";
        int is_low = 0;
        size_t n = strlen(name);
        for (const char *p = low; *p; ) {
            const char *e = strchr(p, ','); size_t len = e ? (size_t)(e - p) : strlen(p);
            if (len == n && !strncmp(p, name, n)) is_low = 1;
            if (!e) break;
            p = e + 1;
        }
        /* the heavy threads (EE, render) sit a little below the light timing ones (vblank, alarm,
           IOP: sound, pads, timers) in the same class, so those preempt them when they wake */
        int heavy = !strncmp(name, "ee", 2) || !strcmp(name, "render");
#if TARGET_OS_IOS
        /* two performance cores on iPhones: the heavy threads (EE, render, GS) are user-initiated,
           the light timing ones (vblank, alarm, IOP: sound, pads, timers) user-interactive, so
           those always get a core when they wake */
        int heavy_ios = heavy || is_low;
        pthread_set_qos_class_self_np(heavy_ios ? QOS_CLASS_USER_INITIATED : QOS_CLASS_USER_INTERACTIVE, 0);
#else
        pthread_set_qos_class_self_np(is_low ? QOS_CLASS_UTILITY : QOS_CLASS_USER_INTERACTIVE, heavy ? -4 : 0);
#endif
    }
    pthread_mutex_lock(&tm);
    if (nt < MAXT) { snprintf(tt[nt].name, sizeof tt[nt].name, "%s", name); tt[nt].port = pthread_mach_thread_np(pthread_self()); nt++; }
    pthread_mutex_unlock(&tm);
}

static double cpu_of(mach_port_t p) {
    thread_basic_info_data_t info; mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    if (thread_info(p, THREAD_BASIC_INFO, (thread_info_t)&info, &n) != KERN_SUCCESS) return 0;
    return info.user_time.seconds + info.user_time.microseconds * 1e-6 + info.system_time.seconds + info.system_time.microseconds * 1e-6;
}

void hp_thread_report(double wall) {
    /* sum by name prefix up to the first '-' digit run, e.g. "ee-3" -> "ee", "gs-1" -> "gs" */
    char names[MAXT][16]; double use[MAXT]; int ng = 0;
    pthread_mutex_lock(&tm);
    for (int k = 0; k < nt; k++) {
        double c = cpu_of(tt[k].port), d = c - tt[k].last; tt[k].last = c;
        char key[16]; snprintf(key, sizeof key, "%s", tt[k].name); char *dash = strchr(key, '-'); if (dash) *dash = 0;
        int g; for (g = 0; g < ng && strcmp(names[g], key); g++) ;
        if (g == ng) { snprintf(names[ng], 16, "%s", key); use[ng++] = 0; }
        use[g] += d;
    }
    pthread_mutex_unlock(&tm);
    LOG("[cpu]");
    for (int g = 0; g < ng; g++) LOG(" %s %.0f%%", names[g], 100 * use[g] / wall);
    LOG("\n");
}
