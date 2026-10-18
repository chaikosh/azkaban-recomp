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
 *   save          as 1, then pause -> Save Game -> format a new card -> slot 1, and exit
 *                 (writes build/memcard_fastboot.ps2 afresh)
 *   load          as menu, then Load Game -> slot 1 of build/memcard_fastboot.ps2
 * Options from HP_ARGS win over the preset; HP_MEMCARD overrides the card file. */
#include "rt.h"
#include "funcs.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

extern unsigned long long g_vsyncs;
double fastboot_wall(void);

#define MAXOPT 32
static struct { char name[48], value[96]; } opts[MAXOPT];
static int nopts = -1;
static int fb_mode, fb_skip, args_log, fs_log;
static const char *fb_name = "";
static const char *ui_script;   /* HP_UISCRIPT or the save/load preset */

/* UI scripts of the save / load presets (see "scripted UI input" below) */
static const char save_script[] =
    "wait:SetTraceControlMode(ig) press:start=PauseGame "              /* pause menu */
    "wait:RequestSaveGameAvailability press:down=ACTION_2 "            /* -> Save Game */
    "press:cross=MemCard(Enable) press:cross=OnSelectSaveGameSlot0 "   /* slot 1 */
    "press:up=ACTION_2 press:cross=OnOption1 "                         /* format? Yes */
    "press:up=ACTION_2 press:cross=OnOption1 "                         /* are you sure? Yes */
    "wait:MemCard(CheckCard) sleep:1 exit";                            /* saved: back on the slot list */
static const char load_script[] =
    "wait:SetTraceControlMode(fe) press:down=ACTION_2 "                /* main menu -> Load Game */
    "press:cross=MemCard(Enable) wait:MemCard(CheckCard) wait:MemCard(CheckCard) "
    "press:cross=OnSelectLoadGameSlot0";                               /* slot 1 */

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
    ui_script = getenv("HP_UISCRIPT");
    add_args(getenv("HP_ARGS"));   /* first: these win over the preset */
    const char *fb = getenv("HP_FASTBOOT");
    if (fb && *fb && strcmp(fb, "0")) {
        fb_mode = 1;
        fb_name = fb;
        fb_skip = strcmp(fb, "noskip") != 0;
        int fe = !strcmp(fb, "menu") || !strcmp(fb, "load");
        if (fe) add_args("-nomovie");
        else add_args("-startChapter=Intro -startRoom=HogwartsExpress");
        if (!ui_script && !strcmp(fb, "save")) ui_script = save_script;
        if (!ui_script && !strcmp(fb, "load")) ui_script = load_script;
        LOG("[fastboot] HP_FASTBOOT=%s: %s%s, cutscenes %s\n", fb, fe ? "front end without movies" :
            "straight into chapter Intro / room HogwartsExpress", !strcmp(fb, "save") ? ", then save to a new card" :
            !strcmp(fb, "load") ? ", then Load Game slot 1" : "", fb_skip ? "skipped" : "played");
    }
    for (int k = 0; k < nopts; k++) LOG("[fastboot] option -%s%s%s\n", opts[k].name, opts[k].value[0] ? "=" : "", opts[k].value);
}

int fastboot_mode(void) { fb_init(); return fb_mode; }

/* Memory card file: HP_MEMCARD, else build/memcard0.ps2; the save/load presets use their own card,
   and save starts it afresh. */
const char *fastboot_memcard(void) {
    fb_init();
    if (getenv("HP_MEMCARD")) return getenv("HP_MEMCARD");
    static const char own[] = "build/memcard_fastboot.ps2";
    if (!strcmp(fb_name, "save")) { unlink(own); return own; }
    if (!strcmp(fb_name, "load")) {
        if (access(own, R_OK)) LOG("[fastboot] %s is missing: make it with HP_FASTBOOT=save\n", own);
        return own;
    }
    return "build/memcard0.ps2";
}

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
void fastboot_note(const char *event);
void hle_fscommand(EE *c) {
    fb_init();
    if (fs_log || ui_script || fb_mode) {
        char name[64], arg[64], ev[132];
        guest_cstr(RW(4), name, sizeof name);
        guest_cstr(RW(5), arg, sizeof arg);
        if (fs_log) LOG("[fscmd v%llu] %s(%s)\n", g_vsyncs, name, arg);
        if (ui_script) { snprintf(ev, sizeof ev, "%s(%s)", name, arg); fastboot_note(ev); }
        /* when the in-game UI and then the HUD (player control) come up */
        if (fb_mode && !strcmp(name, "SetTraceControlMode") && (!strcmp(arg, "pre_ig") || !strcmp(arg, "ig")))
            LOG("[fastboot %.1fs v%llu] %s\n", fastboot_wall(), g_vsyncs, arg[0] == 'p' ? "in-game UI starting (pre_ig)" : "in-game HUD, player control (ig)");
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
static uint16_t ui_buttons(void);
uint16_t fastboot_buttons(void) {
    fb_init();
    if (!fb_mode && !fs_log && !ui_script) return 0;
    uint16_t ui = ui_buttons();
    static uint32_t cur; static unsigned long long since; static int ended;
    uint32_t scene = ld32(ORC_INIT) ? ld32(ORC_OBJ + 140) : 0;
    if (scene != cur) {
        if (!scene) {
            LOG("[fastboot %.1fs v%llu] cutscene over%s\n", fastboot_wall(), g_vsyncs, ended ? "" : ": gameplay");
            ended = 1;
        }
        cur = scene; since = g_vsyncs;
    }
    if (!fb_skip || !scene || !ld32(ORC_OBJ + 156)) return ui;
    unsigned long long d = g_vsyncs - since;
    return ui | (d >= 15 && (d / 12) % 2 == 1 ? 1u << 14 : 0);   /* Cross: 12 vsyncs down, 12 up */
}

/* Called at each vblank: headless runs poll no input, so the fast boot sets the pad here. */
void fastboot_vblank(void) {
    static int headless = -1;
    if (headless < 0) headless = getenv("HP_HEADLESS") != NULL;
    if (!headless || (!fastboot_mode() && !ui_script)) return;
    extern int iopb_enabled(void);
    extern void iopb_set_pad(int, uint16_t, const uint8_t *);
    static const uint8_t axes[4] = {0x80, 0x80, 0x80, 0x80};
    if (iopb_enabled()) iopb_set_pad(0, fastboot_buttons(), axes);
}

/* ---- scripted UI input (HP_UISCRIPT, HP_FASTBOOT=save/load) ------------------------------------
 * Menus here accept input only once their animations are done, and their delays vary from run to
 * run, so fixed-time key presses (HP_AUTOKEY) are brittle.  A UI script instead reacts to what
 * the GUI reports: the FSCommands it sends (as HP_FSLOG prints them, "name(arg)") and
 * "card_written" when the memory card file has been rewritten.  Whitespace-separated steps:
 *   wait:TEXT        until an event containing TEXT
 *   press:BTN=TEXT   hold BTN 0.75 s, release 0.75 s, again until an event containing TEXT
 *   press:BTN        one press
 *   sleep:SECONDS    emulated seconds
 *   exit             end the run
 * BTN: cross circle square triangle start select up down left right lone rone.  Events count
 * from the one that completed the previous wait/press step on. */
#define NEV 256
static char ev_text[NEV][132];
static unsigned ev_count;                     /* events noted so far (ring index = n % NEV) */
static pthread_mutex_t ev_lock = PTHREAD_MUTEX_INITIALIZER;

void fastboot_note(const char *event) {
    pthread_mutex_lock(&ev_lock);
    snprintf(ev_text[ev_count % NEV], sizeof ev_text[0], "%s", event);
    ev_count++;
    pthread_mutex_unlock(&ev_lock);
}

/* first event index >= *from containing text; -1 if none yet */
static long ev_find(unsigned from, const char *text, size_t len) {
    long hit = -1;
    pthread_mutex_lock(&ev_lock);
    if (ev_count - from > NEV) from = ev_count - NEV;
    for (unsigned n = from; n < ev_count; n++) {
        const char *e = ev_text[n % NEV];
        for (const char *p = e; *p; p++)
            if (!strncmp(p, text, len)) { hit = (long)n; break; }
        if (hit >= 0) break;
    }
    pthread_mutex_unlock(&ev_lock);
    return hit;
}

static int ui_button_bit(const char *n, size_t len) {
    static const char *names[16] = {"select", "lthree", "rthree", "start", "up", "right", "down", "left",
                                    "ltwo", "rtwo", "lone", "rone", "triangle", "circle", "cross", "square"};
    for (int b = 0; b < 16; b++) if (strlen(names[b]) == len && !strncmp(names[b], n, len)) return b;
    return -1;
}

static uint16_t ui_buttons(void) {
    static const char *pc;                 /* current step */
    static unsigned cursor;                /* events from here on count */
    static unsigned long long t0;          /* vsync the step (or its current press) began */
    static int started;
    if (!ui_script) return 0;
    if (!pc) { pc = ui_script; t0 = g_vsyncs; }
    for (;;) {
        while (*pc == ' ' || *pc == '\t' || *pc == '\n') pc++;
        if (!*pc) return 0;
        const char *end = pc; while (*end && *end != ' ' && *end != '\t' && *end != '\n') end++;
        const char *colon = memchr(pc, ':', (size_t)(end - pc));
        const char *arg = colon ? colon + 1 : end;
        if (!started) { LOG("[ui %.1fs v%llu] %.*s\n", fastboot_wall(), g_vsyncs, (int)(end - pc), pc); started = 1; t0 = g_vsyncs; }
#define NEXT() do { pc = end; started = 0; } while (0)
        if (!strncmp(pc, "wait:", 5)) {
            long hit = ev_find(cursor, arg, (size_t)(end - arg));
            if (hit < 0) return 0;
            cursor = (unsigned)hit + 1; NEXT(); continue;
        }
        if (!strncmp(pc, "sleep:", 6)) {
            if (g_vsyncs - t0 < (unsigned long long)(atof(arg) * 59.94)) return 0;
            NEXT(); continue;
        }
        if (!strncmp(pc, "exit", 4)) {
            extern int iopb_enabled(void); extern void iopb_mc_flush(void);
            if (iopb_enabled()) iopb_mc_flush();
            LOG("[ui %.1fs v%llu] script done, exiting\n", fastboot_wall(), g_vsyncs);
            fflush(NULL); _exit(0);
        }
        if (!strncmp(pc, "press:", 6)) {
            const char *eq = memchr(arg, '=', (size_t)(end - arg));
            int b = ui_button_bit(arg, (size_t)((eq ? eq : end) - arg));
            if (b < 0) { LOG("[ui] unknown button in %.*s\n", (int)(end - pc), pc); NEXT(); continue; }
            unsigned long long d = g_vsyncs - t0;
            if (d < 45) return (uint16_t)(1u << b);       /* held */
            if (d < 90) return 0;                         /* released */
            if (!eq) { NEXT(); continue; }                /* single press done */
            long hit = ev_find(cursor, eq + 1, (size_t)(end - eq - 1));
            if (hit >= 0) { cursor = (unsigned)hit + 1; NEXT(); continue; }
            t0 = g_vsyncs;                                /* not acknowledged yet: press again */
            return (uint16_t)(1u << b);
        }
        LOG("[ui] unknown step %.*s\n", (int)(end - pc), pc);
        NEXT();
#undef NEXT
    }
}
