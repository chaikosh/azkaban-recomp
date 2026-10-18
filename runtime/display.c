/* Window, presentation of the GS display buffer, and keyboard/gamepad input. */
#include "rt.h"
#include "gs.h"
#include <SDL3/SDL.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#define MAXW 1024
#define MAXH 1024
static uint32_t frame_buf[MAXW * MAXH];
static int frame_w = 640, frame_h = 448, frame_new;
static pthread_mutex_t fm = PTHREAD_MUTEX_INITIALIZER;
unsigned long long g_vsyncs;

static inline uint64_t fld(uint64_t v, int lo, int n) { return (v >> lo) & ((1ull << n) - 1); }

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
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t v = gs_read_raw(fbp, fbw, psm, dbx + (uint32_t)x, dby + (uint32_t)y), c;
            if (psm == PSMCT16 || psm == PSMCT16S)
                c = ((v & 0x1f) << 3) | (((v >> 5) & 0x1f) << 11) | (((v >> 10) & 0x1f) << 19);
            else c = v & 0xffffff;
            frame_buf[y * w + x] = c | 0xff000000u;
        }
    frame_w = w; frame_h = h; frame_new = 1;
    /* HP_SHOT=dir: write frame_<vsync>.ppm every 60 vsyncs */
    static const char *shot = (const char *)1;
    if (shot == (const char *)1) shot = getenv("HP_SHOT");
    if (shot && g_vsyncs % (getenv("HP_SHOTEVERY") ? atoi(getenv("HP_SHOTEVERY")) : 60) == 0) {
        char path[1100];
        snprintf(path, sizeof path, "%s/frame_%05llu.ppm", shot, g_vsyncs);
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", w, h);
            for (int i = 0; i < w * h; i++) { uint32_t c = frame_buf[i]; uint8_t px[3] = {(uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16)}; fwrite(px, 1, 3, f); }
            fclose(f);
        }
    }
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
    uint16_t on = 0;
    /* HP_AUTOKEY="start@8,cross@12": press a button for ~0.3 s at the given second */
    static const char *ak = (const char *)1;
    static double t0 = -1;
    if (ak == (const char *)1) ak = getenv("HP_AUTOKEY");
    if (ak) {
        double now = (double)SDL_GetTicks() / 1000.0;
        if (t0 < 0) t0 = now;
        const char *p = ak;
        while (*p) {
            char name[16]; double at = 0;
            if (sscanf(p, "%15[a-z]@%lf", name, &at) == 2 && now - t0 >= at && now - t0 < at + 0.3) {
                int b = !strcmp(name, "start") ? B_START : !strcmp(name, "cross") ? B_CRO : !strcmp(name, "circle") ? B_CIR :
                        !strcmp(name, "select") ? B_SELECT : !strcmp(name, "down") ? B_DOWN : !strcmp(name, "up") ? B_UP : B_START;
                on |= 1u << b;
            }
            p = strchr(p, ',');
            if (!p) break;
            p++;
        }
    }
#define KEY(sc, b) if (k[sc]) on |= 1u << (b)
    KEY(SDL_SCANCODE_RETURN, B_START); KEY(SDL_SCANCODE_RSHIFT, B_SELECT);
    KEY(SDL_SCANCODE_UP, B_UP); KEY(SDL_SCANCODE_DOWN, B_DOWN); KEY(SDL_SCANCODE_LEFT, B_LEFT); KEY(SDL_SCANCODE_RIGHT, B_RIGHT);
    KEY(SDL_SCANCODE_Z, B_CRO); KEY(SDL_SCANCODE_X, B_CIR); KEY(SDL_SCANCODE_A, B_SQU); KEY(SDL_SCANCODE_S, B_TRI);
    KEY(SDL_SCANCODE_Q, B_L1); KEY(SDL_SCANCODE_E, B_R1); KEY(SDL_SCANCODE_1, B_L2); KEY(SDL_SCANCODE_3, B_R2);
#undef KEY
    int lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
    if (k[SDL_SCANCODE_LEFT]) lx = 0; if (k[SDL_SCANCODE_RIGHT]) lx = 0xff;
    if (k[SDL_SCANCODE_UP]) ly = 0; if (k[SDL_SCANCODE_DOWN]) ly = 0xff;
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
    uint8_t axes[4] = {(uint8_t)rx, (uint8_t)ry, (uint8_t)lx, (uint8_t)ly};
    { extern int iopb_enabled(void); extern void iopb_set_pad(int, uint16_t, const uint8_t *);
      if (iopb_enabled()) iopb_set_pad(0, (uint16_t)on, axes); else pad_set_state((uint16_t)~on, axes); }
}

/* SPU2 output from the IOP (48 kHz stereo s16) */
static SDL_AudioStream *g_audio;
static void audio_sink(const int16_t *samples, size_t frames) {
    if (!g_audio) return;
    if (SDL_GetAudioStreamQueued(g_audio) > 48000 * 4 / 2) return;   /* never queue more than 0.5 s */
    SDL_PutAudioStreamData(g_audio, samples, (int)(frames * 4));
}
static void audio_open(void) {
    extern int iopb_enabled(void);
    extern void iopb_set_audio_sink(void (*)(const int16_t *, size_t));
    if (!iopb_enabled() || getenv("HP_MUTE")) return;
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, 48000};
    g_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!g_audio) { LOG("[audio] %s\n", SDL_GetError()); return; }
    SDL_ResumeAudioStreamDevice(g_audio);
    iopb_set_audio_sink(audio_sink);
}

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
            if (e.type == SDL_EVENT_QUIT) { fflush(stderr); _exit(0); }
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
