/* Virtual PS2 controller for touch screens, drawn over the game with SDL_Renderer.
 *
 * Console layout in landscape: L2/L1 and R2/R1 in the top corners, the D-pad on the left with
 * the left analog stick below it, the four face buttons on the right, SELECT and START at the
 * bottom centre.  Sizes are in units of 1% of the screen height; the clusters sit in the bars
 * beside the 4:3 picture on phones and overlap it on iPads.
 *
 * Multi-touch: every finger presses whatever it is over, so a thumb can slide from button to
 * button or rest between two face buttons to press both.  A finger that lands on the stick
 * drives it until it lifts, wherever it moves. */
#include <TargetConditionals.h>
#if TARGET_OS_IOS
#include "touchpad.h"
#include <math.h>
#include <string.h>

/* bit numbers of the pad report, in display.c's order */
enum { B_SELECT = 0, B_L3, B_R3, B_START, B_UP, B_RIGHT, B_DOWN, B_LEFT, B_L2, B_R2, B_L1, B_R1, B_TRI, B_CIR, B_CRO, B_SQU };

typedef struct {
    int bit, round;       /* round: circle centred at (x, y) with radius w; else a w x h rectangle */
    float x, y, w, h;
    const char *label;
} Button;

enum { NBTN = 10, MAXF = 10 };
static Button btn[NBTN];
static float u = 1;                   /* 1% of the screen height, in pixels */
static float dpx, dpy, dpr;           /* D-pad centre and radius */
static float stx, sty, str;           /* stick base centre and radius */
static int scr_w = 1, scr_h = 1;

static struct { SDL_FingerID id; float x, y; int stick; } fin[MAXF];
static int nfin;
static int hidden, pad_seen;
static uint16_t pressed;
static int stick_on;
static float knob_x, knob_y;          /* -1..1 */

void touchpad_layout(SDL_Window *win, int w, int h) {
    if (w <= 0 || h <= 0) return;
    scr_w = w; scr_h = h;
    u = h / 100.0f;
    /* Safe-area insets (rounded corners, Dynamic Island): keep the controls partly inside. */
    float sl = 0, sr = 0;
    SDL_Rect safe;
    int ww = 0, wh = 0;
    if (SDL_GetWindowSafeArea(win, &safe) && SDL_GetWindowSize(win, &ww, &wh) && ww > 0) {
        float k = (float)w / (float)ww;
        sl = safe.x * k;
        sr = (ww - safe.x - safe.w) * k;
    }
    float left = fmaxf(3 * u, sl * 0.6f), right = w - fmaxf(3 * u, sr * 0.6f);
    int n = 0;
    btn[n++] = (Button){B_L2, 0, left, 3 * u, 20 * u, 8 * u, "L2"};
    btn[n++] = (Button){B_L1, 0, left, 13 * u, 20 * u, 8 * u, "L1"};
    btn[n++] = (Button){B_R2, 0, right - 20 * u, 3 * u, 20 * u, 8 * u, "R2"};
    btn[n++] = (Button){B_R1, 0, right - 20 * u, 13 * u, 20 * u, 8 * u, "R1"};
    const float fx = right - 17 * u, fy = 47 * u, d = 10.5f * u, r = 6.5f * u;
    btn[n++] = (Button){B_TRI, 1, fx, fy - d, r, r, NULL};
    btn[n++] = (Button){B_CIR, 1, fx + d, fy, r, r, NULL};
    btn[n++] = (Button){B_CRO, 1, fx, fy + d, r, r, NULL};
    btn[n++] = (Button){B_SQU, 1, fx - d, fy, r, r, NULL};
    btn[n++] = (Button){B_SELECT, 0, w * 0.5f - 21 * u, 91 * u, 16 * u, 6 * u, "SELECT"};
    btn[n++] = (Button){B_START, 0, w * 0.5f + 5 * u, 91 * u, 16 * u, 6 * u, "START"};
    dpx = left + 16 * u; dpy = 45 * u; dpr = 15 * u;
    stx = left + 22 * u; sty = 79 * u; str = 13 * u;
}

static int hit(const Button *b, float x, float y) {
    if (b->round) return hypotf(x - b->x, y - b->y) <= b->w * 1.3f;
    const float m = 2 * u;
    return x >= b->x - m && x <= b->x + b->w + m && y >= b->y - m && y <= b->y + b->h + m;
}

static void update(void) {
    uint16_t on = 0;
    stick_on = 0;
    knob_x = knob_y = 0;
    for (int k = 0; k < nfin; k++) {
        const float x = fin[k].x, y = fin[k].y;
        if (fin[k].stick) {
            float dx = (x - stx) / str, dy = (y - sty) / str, m = hypotf(dx, dy);
            if (m > 1) { dx /= m; dy /= m; }
            knob_x = dx; knob_y = dy; stick_on = 1;
            continue;
        }
        for (int i = 0; i < NBTN; i++)
            if (hit(&btn[i], x, y)) on |= 1u << btn[i].bit;
        const float dx = x - dpx, dy = y - dpy, m = hypotf(dx, dy);
        if (m <= dpr * 1.3f && m >= dpr * 0.15f) {   /* 8 directions: sin(22.5 deg) = 0.38 */
            if (dx > 0.38f * m) on |= 1u << B_RIGHT;
            if (dx < -0.38f * m) on |= 1u << B_LEFT;
            if (dy > 0.38f * m) on |= 1u << B_DOWN;
            if (dy < -0.38f * m) on |= 1u << B_UP;
        }
    }
    pressed = on;
}

void touchpad_event(const SDL_Event *e) {
    const Uint32 t = e->type;
    if (t != SDL_EVENT_FINGER_DOWN && t != SDL_EVENT_FINGER_MOTION && t != SDL_EVENT_FINGER_UP && t != SDL_EVENT_FINGER_CANCELED)
        return;
    const float x = e->tfinger.x * scr_w, y = e->tfinger.y * scr_h;
    int k = 0;
    while (k < nfin && fin[k].id != e->tfinger.fingerID) k++;
    if (t == SDL_EVENT_FINGER_DOWN) {
        hidden = 0;
        if (k == nfin) {
            if (nfin == MAXF) return;
            nfin++;
        }
        fin[k].id = e->tfinger.fingerID;
        fin[k].x = x; fin[k].y = y;
        const float ds = hypotf(x - stx, y - sty), dd = hypotf(x - dpx, y - dpy);
        fin[k].stick = ds <= str * 1.5f && ds / str <= dd / dpr;
    } else if (k < nfin) {
        if (t == SDL_EVENT_FINGER_MOTION) { fin[k].x = x; fin[k].y = y; }
        else fin[k] = fin[--nfin];
    }
    update();
}

void touchpad_set_gamepad(int connected) {
    if (connected && !pad_seen) hidden = 1;
    if (!connected) hidden = 0;
    pad_seen = connected;
}

uint16_t touchpad_buttons(void) { return pressed; }

int touchpad_stick(uint8_t *x, uint8_t *y) {
    if (!stick_on) return 0;
    float vx = 128 + knob_x * 127.5f, vy = 128 + knob_y * 127.5f;
    *x = (uint8_t)fminf(fmaxf(vx, 0), 255);
    *y = (uint8_t)fminf(fmaxf(vy, 0), 255);
    return 1;
}

/* ---- drawing: batched triangles ------------------------------------------------------------ */
enum { MAXV = 4096, MAXI = 12288 };
static SDL_Vertex vb[MAXV];
static int ib[MAXI];
static int nv, ni;
static SDL_Renderer *R;

static void flush(void) {
    if (ni) SDL_RenderGeometry(R, NULL, vb, nv, ib, ni);
    nv = ni = 0;
}
static void room(int v, int i) { if (nv + v > MAXV || ni + i > MAXI) flush(); }
static int vtx(float x, float y, SDL_FColor c) { vb[nv] = (SDL_Vertex){{x, y}, c, {0, 0}}; return nv++; }
static void tri(int a, int b, int c) { ib[ni++] = a; ib[ni++] = b; ib[ni++] = c; }

static void disc(float cx, float cy, float r, SDL_FColor c) {
    enum { N = 40 };
    room(N + 1, N * 3);
    const int o = vtx(cx, cy, c);
    for (int i = 0; i < N; i++) vtx(cx + r * cosf(i * 6.2831853f / N), cy + r * sinf(i * 6.2831853f / N), c);
    for (int i = 0; i < N; i++) tri(o, o + 1 + i, o + 1 + (i + 1) % N);
}

static void ring(float cx, float cy, float r, float t, SDL_FColor c) {
    enum { N = 40 };
    room(2 * N, 6 * N);
    const int o = nv;
    for (int i = 0; i < N; i++) {
        const float a = i * 6.2831853f / N, ca = cosf(a), sa = sinf(a);
        vtx(cx + r * ca, cy + r * sa, c);
        vtx(cx + (r - t) * ca, cy + (r - t) * sa, c);
    }
    for (int i = 0; i < N; i++) {
        const int j = (i + 1) % N;
        tri(o + 2 * i, o + 2 * j, o + 2 * i + 1);
        tri(o + 2 * i + 1, o + 2 * j, o + 2 * j + 1);
    }
}

/* thick segment with square caps (so polylines close at the corners) */
static void seg(float x0, float y0, float x1, float y1, float t, SDL_FColor c) {
    float dx = x1 - x0, dy = y1 - y0, m = hypotf(dx, dy);
    if (m <= 0) return;
    dx *= t * 0.5f / m; dy *= t * 0.5f / m;
    room(4, 6);
    const int o = vtx(x0 - dx - dy, y0 - dy + dx, c);
    vtx(x1 + dx - dy, y1 + dy + dx, c);
    vtx(x1 + dx + dy, y1 + dy - dx, c);
    vtx(x0 - dx + dy, y0 - dy - dx, c);
    tri(o, o + 1, o + 2);
    tri(o, o + 2, o + 3);
}

static void outline(const float *p, int n, float t, SDL_FColor c) {
    for (int i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        seg(p[2 * i], p[2 * i + 1], p[2 * j], p[2 * j + 1], t, c);
    }
}

static void filled_tri(float x0, float y0, float x1, float y1, float x2, float y2, SDL_FColor c) {
    room(3, 3);
    const int o = vtx(x0, y0, c);
    vtx(x1, y1, c);
    vtx(x2, y2, c);
    tri(o, o + 1, o + 2);
}

/* rounded rectangle: fill plus an inner edge of thickness t (t < corner radius) */
static void rrect(float x, float y, float w, float h, float rad, SDL_FColor fill, SDL_FColor edge, float t) {
    enum { S = 7, N = 4 * (S + 1) };
    if (rad > w / 2) rad = w / 2;
    if (rad > h / 2) rad = h / 2;
    if (t > rad) t = rad;
    const float cx[4] = {x + w - rad, x + w - rad, x + rad, x + rad}, cy[4] = {y + rad, y + h - rad, y + h - rad, y + rad};
    float px[N], py[N], qx[N], qy[N];
    for (int q = 0, k = 0; q < 4; q++)
        for (int s = 0; s <= S; s++, k++) {
            const float a = -1.5707963f + q * 1.5707963f + s * 1.5707963f / S, ca = cosf(a), sa = sinf(a);
            px[k] = cx[q] + rad * ca; py[k] = cy[q] + rad * sa;
            qx[k] = cx[q] + (rad - t) * ca; qy[k] = cy[q] + (rad - t) * sa;
        }
    room(N + 1, N * 3);
    const int o = vtx(x + w / 2, y + h / 2, fill);
    for (int k = 0; k < N; k++) vtx(px[k], py[k], fill);
    for (int k = 0; k < N; k++) tri(o, o + 1 + k, o + 1 + (k + 1) % N);
    room(2 * N, 6 * N);
    const int e = nv;
    for (int k = 0; k < N; k++) { vtx(px[k], py[k], edge); vtx(qx[k], qy[k], edge); }
    for (int k = 0; k < N; k++) {
        const int j = (k + 1) % N;
        tri(e + 2 * k, e + 2 * j, e + 2 * k + 1);
        tri(e + 2 * k + 1, e + 2 * j, e + 2 * j + 1);
    }
}

static void label(const char *s, float cx, float cy, float height, SDL_FColor c) {
    flush();
    const float k = height / SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE;
    const float tw = (float)strlen(s) * SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * k;
    SDL_SetRenderScale(R, k, k);
    SDL_SetRenderDrawColorFloat(R, c.r, c.g, c.b, c.a);
    SDL_RenderDebugText(R, (cx - tw / 2) / k, (cy - height / 2) / k, s);
    SDL_SetRenderScale(R, 1, 1);
}

static const SDL_FColor IDLE = {1, 1, 1, 0.13f}, DOWN = {1, 1, 1, 0.42f}, EDGE = {1, 1, 1, 0.42f}, TEXT = {1, 1, 1, 0.8f};

static void face_symbol(const Button *b) {
    const float x = b->x, y = b->y, r = b->w, t = 0.13f * r;
    switch (b->bit) {
    case B_TRI: {
        const SDL_FColor c = {0.35f, 0.9f, 0.7f, 0.9f};
        const float p[6] = {x, y - 0.5f * r, x + 0.46f * r, y + 0.3f * r, x - 0.46f * r, y + 0.3f * r};
        outline(p, 3, t, c);
        break;
    }
    case B_CIR: ring(x, y, 0.42f * r, t, (SDL_FColor){1, 0.42f, 0.42f, 0.9f}); break;
    case B_CRO: {
        const SDL_FColor c = {0.5f, 0.7f, 1, 0.9f};
        const float a = 0.36f * r;
        seg(x - a, y - a, x + a, y + a, t, c);
        seg(x - a, y + a, x + a, y - a, t, c);
        break;
    }
    case B_SQU: {
        const SDL_FColor c = {1, 0.6f, 0.85f, 0.9f};
        const float a = 0.33f * r, p[8] = {x - a, y - a, x + a, y - a, x + a, y + a, x - a, y + a};
        outline(p, 4, t, c);
        break;
    }
    }
}

void touchpad_draw(SDL_Renderer *ren) {
    if (hidden) return;
    R = ren;
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    const float t = 0.35f * u;
    /* D-pad: four arms around an empty centre, each with an arrow */
    disc(dpx, dpy, dpr, (SDL_FColor){1, 1, 1, 0.06f});
    const float aw = 0.62f * dpr, len = dpr - aw / 2 - 0.04f * dpr, a = 0.17f * dpr, m = dpr - len / 2;
    const struct { int bit; float x, y, w, h, dx, dy; } arm[4] = {
        {B_UP, dpx - aw / 2, dpy - dpr, aw, len, 0, -1},
        {B_DOWN, dpx - aw / 2, dpy + dpr - len, aw, len, 0, 1},
        {B_LEFT, dpx - dpr, dpy - aw / 2, len, aw, -1, 0},
        {B_RIGHT, dpx + dpr - len, dpy - aw / 2, len, aw, 1, 0},
    };
    for (int i = 0; i < 4; i++) {
        rrect(arm[i].x, arm[i].y, arm[i].w, arm[i].h, 0.18f * aw, (pressed >> arm[i].bit & 1) ? DOWN : IDLE, EDGE, t);
        const float cx = dpx + arm[i].dx * m, cy = dpy + arm[i].dy * m, nx = -arm[i].dy, ny = arm[i].dx;
        filled_tri(cx + arm[i].dx * a, cy + arm[i].dy * a, cx - arm[i].dx * a * 0.6f + nx * a, cy - arm[i].dy * a * 0.6f + ny * a,
                   cx - arm[i].dx * a * 0.6f - nx * a, cy - arm[i].dy * a * 0.6f - ny * a, TEXT);
    }
    /* left stick: base and knob */
    disc(stx, sty, str, IDLE);
    ring(stx, sty, str, t, EDGE);
    const float kr = 0.48f * str, travel = str - kr * 0.6f;
    disc(stx + knob_x * travel, sty + knob_y * travel, kr, stick_on ? DOWN : (SDL_FColor){1, 1, 1, 0.25f});
    ring(stx + knob_x * travel, sty + knob_y * travel, kr, t, EDGE);
    /* buttons */
    for (int i = 0; i < NBTN; i++) {
        const Button *b = &btn[i];
        const SDL_FColor f = (pressed >> b->bit & 1) ? DOWN : IDLE;
        if (b->round) {
            disc(b->x, b->y, b->w, f);
            ring(b->x, b->y, b->w, t, EDGE);
            face_symbol(b);
        } else
            rrect(b->x, b->y, b->w, b->h, b->h * 0.5f, f, EDGE, t);
    }
    flush();
    for (int i = 0; i < NBTN; i++)
        if (btn[i].label) label(btn[i].label, btn[i].x + btn[i].w / 2, btn[i].y + btn[i].h / 2, btn[i].h * 0.42f, TEXT);
}
#endif
