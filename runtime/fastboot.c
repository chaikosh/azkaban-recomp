/* Fast test boot (HP_FASTBOOT), extra game options (HP_ARGS) and cutscene skipping.
 *
 * The game reads its "command line" from RUNTIME.CFG and looks options up with
 * NRuntime::Get(name, buf, len) (f_003480d0).  Its debug options still work:
 *   -startChapter=X -startRoom=Y  the global SkippingFE becomes "true": no logos, movies or
 *                                 front end; the game starts chapter X in room Y
 *                                 (f_0010f328 -> f_0028a7a8(chapter, room, debugpoint))
 *   -startDebugPoint=Z            optional start point inside that room
 *   -nomovie                      the movie player opens nothing (EA/WB logos, intro FMV)
 * HP_ARGS adds options ("-a -b=c ..."); HP_FASTBOOT adds a preset:
 *   1 (or train)  -startChapter=Intro -startRoom=HogwartsExpress, cutscenes skipped
 *   noskip        the same start, cutscenes play
 *   menu          -nomovie: the front end without the FMV, cutscenes skipped
 * Options from HP_ARGS win over the preset. */
#include "rt.h"
#include "funcs.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

extern unsigned long long g_vsyncs;

#define MAXOPT 32
static struct { char name[48], value[96]; } opts[MAXOPT];
static int nopts = -1;
static int fb_mode, fb_skip, args_log, fs_log;

static void add_args(const char *s) {
    while (s && *s && nopts < MAXOPT) {
        while (*s == ' ' || *s == '\t' || *s == '\n') s++;
        if (!*s) break;
        if (*s == '-') s++;
        const char *e = s;
        while (*e && *e != ' ' && *e != '\t' && *e != '\n') e++;
        const char *eq = memchr(s, '=', (size_t)(e - s));
        size_t nl = (size_t)((eq ? eq : e) - s);
        if (nl && nl < sizeof opts[0].name) {
            memcpy(opts[nopts].name, s, nl); opts[nopts].name[nl] = 0;
            opts[nopts].value[0] = 0;
            if (eq) {
                size_t vl = (size_t)(e - eq - 1);
                if (vl >= sizeof opts[0].value) vl = sizeof opts[0].value - 1;
                memcpy(opts[nopts].value, eq + 1, vl); opts[nopts].value[vl] = 0;
            }
            nopts++;
        }
        s = e;
    }
}

static void fb_init(void) {
    if (nopts >= 0) return;
    nopts = 0;
    args_log = getenv("HP_ARGSLOG") != NULL;
    fs_log = getenv("HP_FSLOG") != NULL;
    add_args(getenv("HP_ARGS"));   /* first: these win over the preset */
    const char *fb = getenv("HP_FASTBOOT");
    if (fb && *fb && strcmp(fb, "0")) {
        fb_mode = 1;
        fb_skip = strcmp(fb, "noskip") != 0;
        if (!strcmp(fb, "menu")) add_args("-nomovie");
        else add_args("-startChapter=Intro -startRoom=HogwartsExpress");
        LOG("[fastboot] HP_FASTBOOT=%s: %s, cutscenes %s\n", fb, !strcmp(fb, "menu") ? "front end without movies" :
            "straight into chapter Intro / room HogwartsExpress", fb_skip ? "skipped" : "played");
    }
    for (int k = 0; k < nopts; k++) LOG("[fastboot] option -%s%s%s\n", opts[k].name, opts[k].value[0] ? "=" : "", opts[k].value);
}

int fastboot_mode(void) { fb_init(); return fb_mode; }

static void guest_cstr(uint32_t a, char *out, int n) {
    for (int i = 0; i < n - 1; i++) { out[i] = a ? (char)ld8(a + i) : 0; if (!out[i]) return; }
    out[n - 1] = 0;
}

/* f_003480d0 NRuntime::Get(name, buf, len): 1 if the option is present; its value goes to buf */
void hle_runtime_get(EE *c) {
    fb_init();
    char name[64];
    guest_cstr(RW(4), name, sizeof name);
    for (int k = 0; k < nopts; k++) {
        if (strcasecmp(opts[k].name, name)) continue;
        uint32_t buf = RW(5); int32_t len = (int32_t)RW(6);
        if (buf && len > 0) {
            int i = 0;
            for (; i < len - 1 && opts[k].value[i]; i++) st8(buf + i, (uint8_t)opts[k].value[i]);
            st8(buf + i, 0);
        }
        if (args_log) LOG("[fastboot] Get(%s) -> '%s' (injected)\n", name, opts[k].value);
        R(2) = 1;
        return;
    }
    f_003480d0_orig(c);
    if (args_log) LOG("[fastboot] Get(%s) -> %d\n", name, (int)RW(2));
}

/* f_0010d2e0: dispatch of an Apt (GUI Flash) FSCommand(name, arg) to the game's natives.
   HP_FSLOG=1 logs them, and the cutscenes. */
void hle_fscommand(EE *c) {
    fb_init();
    if (fs_log) {
        char name[64], arg[64];
        guest_cstr(RW(4), name, sizeof name);
        guest_cstr(RW(5), arg, sizeof arg);
        LOG("[fscmd v%llu] %s(%s)\n", g_vsyncs, name, arg);
    }
    f_0010d2e0_orig(c);
}

/* ---- cutscenes ------------------------------------------------------------------------------
 * The orchestrator (f_0017a640 -> 0x4e2a98) plays the in-engine cutscenes: +140 is the current
 * scene, +156 "skippable".  PlayScene (f_0017ab48) always gets skippable = 0 in this build, so
 * the overlay's skip (OK button -> FSCommand SkipCutscene -> f_0017ac88) is refused.  With
 * HP_FASTBOOT the scenes start skippable and fastboot_buttons() presses Cross while one plays:
 * the game's own skip path ends the scene and runs the script's skip callback. */
#define ORC_OBJ  0x004e2a98u
#define ORC_INIT 0x004e2b44u
static double t_start;
__attribute__((constructor)) static void fb_t0(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); t_start = t.tv_sec + t.tv_nsec * 1e-9; }
/* wall seconds since the process started */
double fastboot_wall(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9 - t_start; }

void hle_orc_play_scene(EE *c) {
    fb_init();
    uint32_t orc = RW(4);
    if (fb_skip) R(8) = 1;
    f_0017ab48_orig(c);
    if (fb_mode || fs_log) {
        char nm[64]; uint32_t scene = ld32(orc + 140);
        guest_cstr(scene ? scene + 36 : 0, nm, sizeof nm);   /* the scene's package path */
        LOG("[fastboot %.1fs v%llu] cutscene %s%s\n", fastboot_wall(), g_vsyncs, nm, ld32(orc + 156) ? " (skippable)" : "");
    }
}

/* Pad buttons (bit numbers as in display.c) the fast boot holds at this instant; also notes the
   end of each cutscene. */
uint16_t fastboot_buttons(void) {
    fb_init();
    if (!fb_mode && !fs_log) return 0;
    static uint32_t cur; static unsigned long long since; static int ended;
    uint32_t scene = ld32(ORC_INIT) ? ld32(ORC_OBJ + 140) : 0;
    if (scene != cur) {
        if (!scene) {
            LOG("[fastboot %.1fs v%llu] cutscene over%s\n", fastboot_wall(), g_vsyncs, ended ? "" : ": gameplay");
            ended = 1;
        }
        cur = scene; since = g_vsyncs;
    }
    if (!fb_skip || !scene || !ld32(ORC_OBJ + 156)) return 0;
    unsigned long long d = g_vsyncs - since;
    return d >= 30 && (d / 12) % 2 == 0 ? 1u << 14 : 0;   /* Cross: 12 vsyncs down, 12 up */
}

/* Called at each vblank: headless runs poll no input, so the fast boot sets the pad here. */
void fastboot_vblank(void) {
    static int headless = -1;
    if (headless < 0) headless = getenv("HP_HEADLESS") != NULL;
    if (!headless || !fastboot_mode()) return;
    extern int iopb_enabled(void);
    extern void iopb_set_pad(int, uint16_t, const uint8_t *);
    static const uint8_t axes[4] = {0x80, 0x80, 0x80, 0x80};
    if (iopb_enabled()) iopb_set_pad(0, fastboot_buttons(), axes);
}
