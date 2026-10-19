/* Window, presentation of the GS display buffer, and keyboard/gamepad input. */
#include "rt.h"
#include "gs.h"
#include <SDL3/SDL.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <TargetConditionals.h>
#if TARGET_OS_IOS
#include "ios/touchpad.h"
#endif

#define MAXW 2048   /* up to 3x GPU render scale (PS2_SCALE) */
#define MAXH 2048
static uint32_t frame_buf[MAXW * MAXH];
static int frame_w = 640, frame_h = 448, frame_new;
static pthread_mutex_t fm = PTHREAD_MUTEX_INITIALIZER;
unsigned long long g_vsyncs;
unsigned g_pace[5];   /* new frames shown after 1, 2, 3, 4+ vblanks (index 0 unused) */

static inline uint64_t fld(uint64_t v, int lo, int n) { return (v >> lo) & ((1ull << n) - 1); }

/* PCRTC merge: with both read circuits on, PMODE.MMOD = 1 (fixed alpha ALP) and SLBG = 0 the GS
   outputs circuit 1 * ALP/255 + circuit 2 * (1 - ALP/255), each circuit placed by its DISPLAY
   DX/DY.  This game shows the same buffer on both, circuit 2 one line lower, ALP = 0x80: the
   deflicker filter, which also averages its ordered dither (DIMX) between neighbouring lines.
   Returns ALP + 1, or 0 when the circuits are not merged (HP_PCRTC=single: only one circuit is
   shown, as before; MMOD = 0, alpha from circuit 1's pixels, is not implemented). */
int display_pcrtc_merge(uint64_t pmode) {
    static int single = -1;
    if (single < 0) { const char *e = getenv("HP_PCRTC"); single = e && !strcmp(e, "single"); }
    if (single || (pmode & 3) != 3 || !((pmode >> 5) & 1) || ((pmode >> 7) & 1)) return 0;
    return (int)fld(pmode, 8, 8) + 1;
}

static inline uint32_t to_rgb(uint32_t v, uint32_t psm) {
    if (psm == PSMCT16 || psm == PSMCT16S) return ((v & 0x1f) << 3) | (((v >> 5) & 0x1f) << 11) | (((v >> 10) & 0x1f) << 19);
    return v & 0xffffff;
}

void display_vsync(void) {
    g_vsyncs++;
    uint64_t pmode = hw_gs_priv(0x0000);
    int en1 = pmode & 1, en2 = (pmode >> 1) & 1;
    if (!en1 && !en2) return;
    uint64_t dispfb = hw_gs_priv(en2 ? 0x0090 : 0x0070), display = hw_gs_priv(en2 ? 0x00a0 : 0x0080);
    uint32_t fbp = fld(dispfb, 0, 9) * 32, fbw = fld(dispfb, 9, 6), psm = fld(dispfb, 15, 5);
    uint32_t dbx = fld(dispfb, 32, 11), dby = fld(dispfb, 43, 11);
    uint32_t magh = fld(display, 23, 4), magv = fld(display, 27, 2);
    uint32_t dw = fld(display, 32, 12), dh = fld(display, 44, 11);
    int w = (int)((dw + 1) / (magh + 1)), h = (int)((dh + 1) / (magv + 1));
    if (w > MAXW) w = MAXW;
    if (h > MAXH) h = MAXH;
    if (w <= 0 || h <= 0) return;
    pthread_mutex_lock(&fm);
    extern int g_gs_gpu, g_gs_mirror;
    extern int gsb_display(uint64_t, uint64_t, const uint64_t *, const uint64_t *, uint32_t *, uint32_t, int *, int *);
    /* HP_SHOT=dir: write frame_<vsync>.ppm every 60 vsyncs */
    static const char *shot = (const char *)1;
    if (shot == (const char *)1) shot = getenv("HP_SHOT");
    /* the first frame shown at or after each multiple of HP_SHOTEVERY (with the GPU renderer a new
       frame is not ready at every vsync) */
    static unsigned long long next_shot;
    static int every = -1;
    if (every < 0) { every = getenv("HP_SHOTEVERY") ? atoi(getenv("HP_SHOTEVERY")) : 60; if (every <= 0) every = 60; next_shot = every; }
    /* HP_GS=both: both renderers' frames of a screenshot are taken with the GS input held, so that
       a buffer being drawn into (loading screens) shows the same packets in both */
    extern pthread_mutex_t g_gs_lock;
    const int mirror_shot = g_gs_mirror && shot && g_vsyncs >= next_shot;
    if (mirror_shot) pthread_mutex_lock(&g_gs_lock);
    if (g_gs_gpu) {   /* the GPU renderer hands back the displayed frame */
        uint64_t fb2[2] = {hw_gs_priv(0x0070), hw_gs_priv(0x0090)}, disp2[2] = {hw_gs_priv(0x0080), hw_gs_priv(0x00a0)};
        if (!gsb_display(pmode, hw_gs_priv(0x0020), fb2, disp2, frame_buf, MAXW * MAXH, &w, &h)) { pthread_mutex_unlock(&fm); return; }
        {   /* frame pacing: vblanks between the frames handed to the window (HP_FPSLOG prints it) */
            extern unsigned g_pace[5]; static unsigned long long last;
            unsigned long long d = g_vsyncs - last; last = g_vsyncs;
            g_pace[d >= 4 ? 4 : d]++;
        }
    } else if (display_pcrtc_merge(pmode)) {   /* both circuits, blended (circuit 1's rectangle) */
        const uint32_t alp = (uint32_t)display_pcrtc_merge(pmode) - 1;
        const uint64_t fb1 = hw_gs_priv(0x0070), d1 = hw_gs_priv(0x0080), fb2 = hw_gs_priv(0x0090), d2 = hw_gs_priv(0x00a0);
        const uint32_t p1 = fld(fb1, 0, 9) * 32, w1 = fld(fb1, 9, 6), s1 = fld(fb1, 15, 5), x1 = fld(fb1, 32, 11), y1 = fld(fb1, 43, 11);
        const uint32_t p2 = fld(fb2, 0, 9) * 32, w2 = fld(fb2, 9, 6), s2 = fld(fb2, 15, 5), x2 = fld(fb2, 32, 11), y2 = fld(fb2, 43, 11);
        const int mh1 = (int)fld(d1, 23, 4) + 1, mv1 = (int)fld(d1, 27, 2) + 1, mh2 = (int)fld(d2, 23, 4) + 1, mv2 = (int)fld(d2, 27, 2) + 1;
        w = (int)((fld(d1, 32, 12) + 1) / mh1); h = (int)((fld(d1, 44, 11) + 1) / mv1);
        if (w > MAXW) w = MAXW;
        if (h > MAXH) h = MAXH;
        const int cw2 = (int)((fld(d2, 32, 12) + 1) / mh2), ch2 = (int)((fld(d2, 44, 11) + 1) / mv2);
        const int ox = ((int)fld(d2, 0, 12) - (int)fld(d1, 0, 12)) / mh2, oy = ((int)fld(d2, 12, 11) - (int)fld(d1, 12, 11)) / mv2;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint32_t c1 = to_rgb(gs_read_raw(p1, w1, s1, x1 + (uint32_t)x, y1 + (uint32_t)y), s1), c = c1;
                const int u = x - ox, v = y - oy;
                if (u >= 0 && v >= 0 && u < cw2 && v < ch2) {
                    const uint32_t c2 = to_rgb(gs_read_raw(p2, w2, s2, x2 + (uint32_t)u, y2 + (uint32_t)v), s2);
                    c = 0;
                    for (int k = 0; k < 24; k += 8)
                        c |= ((((c1 >> k) & 0xff) * alp + ((c2 >> k) & 0xff) * (255 - alp)) / 255) << k;
                }
                frame_buf[y * w + x] = c | 0xff000000u;
            }
    } else
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            frame_buf[y * w + x] = to_rgb(gs_read_raw(fbp, fbw, psm, dbx + (uint32_t)x, dby + (uint32_t)y), psm) | 0xff000000u;
    frame_w = w; frame_h = h; frame_new = 1;
    if (shot && g_vsyncs >= next_shot) {
        next_shot = (g_vsyncs / every + 1) * every;
        char path[1100];
        snprintf(path, sizeof path, "%s/frame_%05llu.ppm", shot, g_vsyncs / every * every);
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", w, h);
            for (int i = 0; i < w * h; i++) { uint32_t c = frame_buf[i]; uint8_t px[3] = {(uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16)}; fwrite(px, 1, 3, f); }
            fclose(f);
        }
        if (g_gs_mirror) {   /* HP_GS=both: the GPU renderer's frame from the same packets, gpu_<vsync>.ppm */
            static uint32_t gbuf[MAXW * MAXH];
            int gw = 0, gh = 0;
            uint64_t fb2[2] = {hw_gs_priv(0x0070), hw_gs_priv(0x0090)}, disp2[2] = {hw_gs_priv(0x0080), hw_gs_priv(0x00a0)};
            if (gsb_display(pmode, hw_gs_priv(0x0020), fb2, disp2, gbuf, MAXW * MAXH, &gw, &gh)) {
                snprintf(path, sizeof path, "%s/gpu_%05llu.ppm", shot, g_vsyncs / every * every);
                if ((f = fopen(path, "wb"))) {
                    fprintf(f, "P6\n%d %d\n255\n", gw, gh);
                    for (int i = 0; i < gw * gh; i++) { uint32_t c = gbuf[i]; uint8_t px[3] = {(uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16)}; fwrite(px, 1, 3, f); }
                    fclose(f);
                }
            }
        }
    }
    if (mirror_shot) pthread_mutex_unlock(&g_gs_lock);
    /* HP_SHOTFB=fbp,fbw,psm,w,h (hex fbp in 2048-word pages): also dump that buffer */
    static const char *fb = (const char *)1;
    if (fb == (const char *)1) fb = getenv("HP_SHOTFB");
    if (shot && fb && g_vsyncs % 60 == 0) {
        unsigned fbp2 = 0, fbw2 = 10, psm2 = 0, w2 = 640, h2 = 448;
        sscanf(fb, "%x,%u,%x,%u,%u", &fbp2, &fbw2, &psm2, &w2, &h2);
        char path[1100];
        snprintf(path, sizeof path, "%s/fb_%05llu.ppm", shot, g_vsyncs);
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", w2, h2);
            for (unsigned y = 0; y < h2; y++)
                for (unsigned x = 0; x < w2; x++) {
                    uint32_t v = gs_read_raw(fbp2 * 32, fbw2, psm2, x, y);
                    uint8_t px[3];
                    if (psm2 == PSMCT16 || psm2 == PSMCT16S) { px[0] = (v & 0x1f) << 3; px[1] = ((v >> 5) & 0x1f) << 3; px[2] = ((v >> 10) & 0x1f) << 3; }
                    else { px[0] = (uint8_t)v; px[1] = (uint8_t)(v >> 8); px[2] = (uint8_t)(v >> 16); }
                    fwrite(px, 1, 3, f);
                }
            fclose(f);
        }
    }
    pthread_mutex_unlock(&fm);
}

void pad_set_state(uint16_t buttons_active_low, const uint8_t axes[4]);

enum { B_SELECT = 0, B_L3, B_R3, B_START, B_UP, B_RIGHT, B_DOWN, B_LEFT, B_L2, B_R2, B_L1, B_R1, B_TRI, B_CIR, B_CRO, B_SQU };

static void poll_input(SDL_Gamepad *pad) {
    const bool *k = SDL_GetKeyboardState(NULL);
    uint16_t on = 0, ak_dirs = 0;
    /* HP_AUTOKEY="start@8,cross@12:1.5": press a button at the given second for 0.3 s (or the given hold time) */
    static const char *ak = (const char *)1;
    static double t0 = -1;
    if (ak == (const char *)1) ak = getenv("HP_AUTOKEY");
    if (ak) {
        /* HP_AUTOKEYV=1: the times are emulated seconds (vsyncs / 59.94) instead of wall time, so runs repeat */
        static int virt = -1; if (virt < 0) virt = getenv("HP_AUTOKEYV") != NULL;
        double now = virt ? (double)g_vsyncs / 59.94 : (double)SDL_GetTicks() / 1000.0;
        if (t0 < 0) t0 = now;
        const char *p = ak;
        while (*p) {
            char name[16]; double at = 0, hold = 0.3;   /* "name@second" or "name@second:holdseconds" */
            if (sscanf(p, "%15[a-z]@%lf:%lf", name, &at, &hold) >= 2 && now - t0 >= at && now - t0 < at + hold) {
                int b = !strcmp(name, "start") ? B_START : !strcmp(name, "cross") ? B_CRO : !strcmp(name, "circle") ? B_CIR :
                        !strcmp(name, "select") ? B_SELECT : !strcmp(name, "down") ? B_DOWN : !strcmp(name, "up") ? B_UP :
                        !strcmp(name, "left") ? B_LEFT : !strcmp(name, "right") ? B_RIGHT : !strcmp(name, "triangle") ? B_TRI :
                        !strcmp(name, "square") ? B_SQU : !strcmp(name, "lone") ? B_L1 : !strcmp(name, "rone") ? B_R1 : B_START;
                int stick = !strcmp(name, "lleft") ? 1 : !strcmp(name, "lright") ? 2 : !strcmp(name, "lup") ? 4 : !strcmp(name, "ldown") ? 8 : 0;
                if (stick) ak_dirs |= stick; else on |= 1u << b;
            }
            p = strchr(p, ',');
            if (!p) break;
            p++;
        }
    }
    { extern uint16_t fastboot_buttons(void); on |= fastboot_buttons(); }   /* HP_FASTBOOT: skips cutscenes */
#define KEY(sc, b) if (k[sc]) on |= 1u << (b)
    KEY(SDL_SCANCODE_RETURN, B_START); KEY(SDL_SCANCODE_RSHIFT, B_SELECT);
    KEY(SDL_SCANCODE_UP, B_UP); KEY(SDL_SCANCODE_DOWN, B_DOWN); KEY(SDL_SCANCODE_LEFT, B_LEFT); KEY(SDL_SCANCODE_RIGHT, B_RIGHT);
    KEY(SDL_SCANCODE_Z, B_CRO); KEY(SDL_SCANCODE_X, B_CIR); KEY(SDL_SCANCODE_A, B_SQU); KEY(SDL_SCANCODE_S, B_TRI);
    KEY(SDL_SCANCODE_Q, B_L1); KEY(SDL_SCANCODE_E, B_R1); KEY(SDL_SCANCODE_1, B_L2); KEY(SDL_SCANCODE_3, B_R2);
#undef KEY
    int lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
    if (k[SDL_SCANCODE_LEFT]) lx = 0; if (k[SDL_SCANCODE_RIGHT]) lx = 0xff;
    if (k[SDL_SCANCODE_UP]) ly = 0; if (k[SDL_SCANCODE_DOWN]) ly = 0xff;
    /* scripted left stick: "lup", "ldown", "lleft", "lright" */
    if (ak_dirs & 1) lx = 0; if (ak_dirs & 2) lx = 0xff;
    if (ak_dirs & 4) ly = 0; if (ak_dirs & 8) ly = 0xff;
    if (k[SDL_SCANCODE_J]) rx = 0; if (k[SDL_SCANCODE_L]) rx = 0xff;
    if (k[SDL_SCANCODE_I]) ry = 0; if (k[SDL_SCANCODE_K]) ry = 0xff;
    if (pad) {
#define GB(g, b) if (SDL_GetGamepadButton(pad, g)) on |= 1u << (b)
        GB(SDL_GAMEPAD_BUTTON_SOUTH, B_CRO); GB(SDL_GAMEPAD_BUTTON_EAST, B_CIR); GB(SDL_GAMEPAD_BUTTON_WEST, B_SQU);
        GB(SDL_GAMEPAD_BUTTON_NORTH, B_TRI); GB(SDL_GAMEPAD_BUTTON_START, B_START); GB(SDL_GAMEPAD_BUTTON_BACK, B_SELECT);
        GB(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, B_L1); GB(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, B_R1);
        GB(SDL_GAMEPAD_BUTTON_LEFT_STICK, B_L3); GB(SDL_GAMEPAD_BUTTON_RIGHT_STICK, B_R3);
        GB(SDL_GAMEPAD_BUTTON_DPAD_UP, B_UP); GB(SDL_GAMEPAD_BUTTON_DPAD_DOWN, B_DOWN);
        GB(SDL_GAMEPAD_BUTTON_DPAD_LEFT, B_LEFT); GB(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, B_RIGHT);
#undef GB
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) on |= 1u << B_L2;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) on |= 1u << B_R2;
        int ax[4] = {SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX), SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY),
                     SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY)};
        int *dst[4] = {&rx, &ry, &lx, &ly};
        for (int i = 0; i < 4; i++)
            if (ax[i] > 4000 || ax[i] < -4000) *dst[i] = (ax[i] + 32768) >> 8;
    }
#if TARGET_OS_IOS
    on |= touchpad_buttons();
    { uint8_t tx, ty; if (touchpad_stick(&tx, &ty)) { lx = tx; ly = ty; } }
    { uint8_t tx, ty; if (touchpad_rstick(&tx, &ty)) { rx = tx; ry = ty; } }
#endif
    uint8_t axes[4] = {(uint8_t)rx, (uint8_t)ry, (uint8_t)lx, (uint8_t)ly};
    { extern int iopb_enabled(void); extern void iopb_set_pad(int, uint16_t, const uint8_t *);
      if (iopb_enabled()) iopb_set_pad(0, (uint16_t)on, axes); else pad_set_state((uint16_t)~on, axes); }
}

/* SPU2 output from the IOP (48 kHz stereo s16) */
static SDL_AudioStream *g_audio;
/* The IOP produces sound in short bursts as it catches up with the emulated clock, so the stream
   keeps a margin: playback (re)starts only once ~100 ms is queued, and pauses when the queue runs
   dry instead of stuttering on every late burst.  g_audio_underruns counts the dry spells. */
unsigned g_audio_underruns;
/* for HP_FPSLOG: frames produced, dropped (queue full), and the queue's low and high water marks */
unsigned long long g_audio_frames, g_audio_dropped;
int g_audio_qmin = 1 << 30, g_audio_qmax;
static int g_audio_paused = 1;
static void audio_sink(const int16_t *samples, size_t frames) {
    if (!g_audio) return;
    int queued = SDL_GetAudioStreamQueued(g_audio);
    g_audio_frames += frames;
    if (queued < g_audio_qmin) g_audio_qmin = queued;
    if (queued > g_audio_qmax) g_audio_qmax = queued;
    if (queued > 48000 * 4 / 2) { g_audio_dropped += frames; return; }   /* never queue more than 0.5 s */
    if (!g_audio_paused && queued == 0) {
        SDL_PauseAudioStreamDevice(g_audio);
        g_audio_paused = 1;
        g_audio_underruns++;
    }
    SDL_PutAudioStreamData(g_audio, samples, (int)(frames * 4));
#if TARGET_OS_IOS
    const int margin = 48000 * 4 * 15 / 100;   /* 150 ms on phones */
#else
    const int margin = 48000 * 4 / 10;         /* 100 ms */
#endif
    if (g_audio_paused && queued + (int)(frames * 4) >= margin) {
        SDL_ResumeAudioStreamDevice(g_audio);
        g_audio_paused = 0;
    }
}
static void audio_open(void) {
    extern int iopb_enabled(void);
    extern void iopb_set_audio_sink(void (*)(const int16_t *, size_t));
    if (!iopb_enabled() || getenv("HP_MUTE")) return;
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, 48000};
    g_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!g_audio) { LOG("[audio] %s\n", SDL_GetError()); return; }
    iopb_set_audio_sink(audio_sink);   /* playback starts once the margin is queued */
}

#if TARGET_OS_IOS
/* iOS: full-screen landscape, the picture at 4:3 in the middle, the touch controller on top.
   Between resign-active and become-active the GS is held (gsb_pause): iOS forbids GPU work in
   the background, and every guest thread that reaches the GS parks there. */
static volatile int g_paused;
static bool SDLCALL app_events(void *userdata, SDL_Event *e) {
    (void)userdata;
    extern void gsb_pause(int);
    if (e->type == SDL_EVENT_WILL_ENTER_BACKGROUND && !g_paused) { g_paused = 1; gsb_pause(1); LOG("[ios] paused\n"); }
    else if (e->type == SDL_EVENT_DID_ENTER_FOREGROUND && g_paused) { gsb_pause(0); g_paused = 0; LOG("[ios] resumed\n"); }
    return true;
}

void display_loop(void) {
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "2");   /* hidden; edge swipes reach the game first */
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) { LOG("[display] SDL_Init: %s\n", SDL_GetError()); return; }
    audio_open();
    SDL_AddEventWatch(app_events, NULL);
    SDL_Window *win = SDL_CreateWindow("Harry Potter and the Prisoner of Azkaban", 0, 0,
                                       SDL_WINDOW_FULLSCREEN | SDL_WINDOW_BORDERLESS | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer *ren = SDL_CreateRenderer(win, NULL);
    SDL_SetRenderVSync(ren, 1);
    SDL_Texture *tex = NULL;
    int tw = 0, th = 0;
    SDL_Gamepad *pad = NULL;
    static uint32_t local[MAXW * MAXH];
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) { LOG("[display] quit event\n"); fflush(stderr); _exit(0); }
            if (e.type == SDL_EVENT_GAMEPAD_ADDED && !pad) pad = SDL_OpenGamepad(e.gdevice.which);
            if (e.type == SDL_EVENT_GAMEPAD_REMOVED && pad && e.gdevice.which == SDL_GetGamepadID(pad)) { SDL_CloseGamepad(pad); pad = NULL; }
            touchpad_event(&e);
        }
        if (g_paused) { SDL_Delay(50); continue; }
        int ow = 0, oh = 0;
        SDL_GetCurrentRenderOutputSize(ren, &ow, &oh);
        touchpad_layout(win, ow, oh);
        touchpad_set_gamepad(pad != NULL);
        poll_input(pad);
        int w = 0, h = 0, have = 0;
        pthread_mutex_lock(&fm);
        if (frame_new) {
            w = frame_w; h = frame_h;
            memcpy(local, frame_buf, (size_t)w * h * 4);
            frame_new = 0; have = 1;
        }
        pthread_mutex_unlock(&fm);
        if (have) {
            if (!tex || tw != w || th != h) {
                if (tex) SDL_DestroyTexture(tex);
                tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w, h);
                tw = w; th = h;
            }
            SDL_UpdateTexture(tex, NULL, local, w * 4);
        }
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        if (tex) {   /* the PS2 picture is 4:3 whatever its resolution */
            float dw = (float)ow, dh = (float)oh;
            if (dw * 3 > dh * 4) dw = dh * 4 / 3; else dh = dw * 3 / 4;
            const SDL_FRect dst = {(ow - dw) / 2, (oh - dh) / 2, dw, dh};
            SDL_RenderTexture(ren, tex, NULL, &dst);
        }
        touchpad_draw(ren);
        SDL_RenderPresent(ren);   /* paced by vsync */
    }
}
#else
void display_loop(void) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) { LOG("[display] SDL_Init: %s\n", SDL_GetError()); return; }
    audio_open();
    SDL_Window *win = SDL_CreateWindow("Harry Potter and the Prisoner of Azkaban", 960, 672, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = SDL_CreateRenderer(win, NULL);
    SDL_Texture *tex = NULL;
    int tw = 0, th = 0;
    SDL_Gamepad *pad = NULL;
    static uint32_t local[MAXW * MAXH];
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_LOW_MEMORY) LOG("[ios] low memory warning\n");
            if (e.type == SDL_EVENT_TERMINATING) LOG("[ios] terminating\n");
            if (e.type == SDL_EVENT_QUIT) {   /* a save made just before must still reach the card file */
                LOG("[ios] quit event\n");
                extern int iopb_enabled(void); extern void iopb_mc_flush(void);
                if (iopb_enabled()) iopb_mc_flush();
                fflush(stderr); _exit(0);
            }
            if (e.type == SDL_EVENT_GAMEPAD_ADDED && !pad) pad = SDL_OpenGamepad(e.gdevice.which);
        }
        poll_input(pad);
        int w = 0, h = 0, have = 0;
        pthread_mutex_lock(&fm);
        if (frame_new) {
            w = frame_w; h = frame_h;
            memcpy(local, frame_buf, (size_t)w * h * 4);
            frame_new = 0; have = 1;
        }
        pthread_mutex_unlock(&fm);
        if (have) {
            if (!tex || tw != w || th != h) {
                if (tex) SDL_DestroyTexture(tex);
                tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w, h);
                tw = w; th = h;
            }
            SDL_UpdateTexture(tex, NULL, local, w * 4);
            SDL_RenderClear(ren);
            SDL_RenderTexture(ren, tex, NULL, NULL);
            SDL_RenderPresent(ren);
        } else SDL_Delay(2);
    }
}
#endif
