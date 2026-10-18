// GPU renderer for the hp runtime (HP_GS=gpu): ps2-web-recomp's GS (vendored in third_party/ps2gs,
// MIT) draws with WebGPU compute shaders through wgpu-native (Metal on Apple platforms).
//
// hp's own GS front end (runtime/gs.c) still parses every GIF packet for the privileged side
// (SIGNAL/FINISH/LABEL, CSR, interrupts) but stops rasterizing; the same packets are handed here
// in the same order.  Downloads (local->host transfers) and the displayed frame come back from the
// GPU.  The vendored GS is single-threaded, so every entry point takes one lock.

#include "gs/gif.hpp"
#include "gs/gs.hpp"
#include "gs/gpu/gpu.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

extern "C" unsigned long long g_vsyncs;
namespace ps2 {
std::uint64_t frame_count() { return g_vsyncs; }
}

namespace ps2::gs::gpu {
extern std::uint64_t g_hp_image_calls, g_hp_image_bytes, g_hp_emits, g_hp_pre_total, g_hp_pre_max;
}

namespace {
std::mutex g_lock;
bool g_ready = false;
std::vector<std::uint32_t> g_rgba;
double g_gif_s = 0, g_display_s = 0, g_download_s = 0;

double secs() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// GIF data often arrives one quadword at a time (GIF FIFO writes, VIF DIRECT); the GPU code
// queues one upload per call, so consecutive data on the same path is gathered first.  A single
// pending run keeps the global order of the paths.
std::vector<std::uint8_t> g_pend;
int g_pend_path = -1;

void flush_pending() {
    if (g_pend.empty()) return;
    const double t0 = secs();
    ps2::gif::render(g_pend_path + 1, g_pend.data(), static_cast<std::uint32_t>(g_pend.size() / 16));
    g_pend.clear();
    g_gif_s += secs() - t0;
}
}

extern "C" {

int gsb_enabled(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = std::getenv("HP_GS");
        on = e && std::string(e) == "gpu";
    }
    return on;
}

/* Returns 0 when WebGPU is unavailable (the caller then keeps the software renderer). */
int gsb_init(void) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_ready) return 1;
    ps2::gs::init();
    if (ps2::gs::gpu::mode() != ps2::gs::gpu::Mode::primary) {
        std::fprintf(stderr, "[gsgpu] WebGPU unavailable, keeping the software GS\n");
        return 0;
    }
    ps2::gs::reset();
    ps2::gif::reset();
    g_ready = true;
    std::fprintf(stderr, "[gsgpu] GPU renderer active\n");
    return 1;
}

/* GIF data in hp's path numbering (0 = PATH1/VU1, 1 = PATH2/VIF1 DIRECT, 2 = PATH3/GIF DMA). */
void gsb_gif(int path, const void *data, uint32_t qwc) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (path != g_pend_path) {
        flush_pending();
        g_pend_path = path;
    }
    const auto *p = static_cast<const std::uint8_t *>(data);
    g_pend.insert(g_pend.end(), p, p + static_cast<std::size_t>(qwc) * 16);
    if (g_pend.size() >= (1u << 20)) flush_pending();
}

/* Next `qwords` of a local->host transfer started by TRXDIR=1. */
void gsb_read_image(uint8_t *dst, uint32_t qwords) {
    std::lock_guard<std::mutex> lk(g_lock);
    const double t0 = secs();
    flush_pending();
    ps2::gs::read_image(dst, qwords);
    g_download_s += secs() - t0;
}

/* The displayed frame as RGBA (R in the low byte), at most max_pixels; returns 0 if nothing is shown. */
int gsb_display(uint64_t pmode, uint64_t smode2, const uint64_t dispfb[2], const uint64_t display[2], uint32_t *out,
                uint32_t max_pixels, int *w, int *h) {
    std::lock_guard<std::mutex> lk(g_lock);
    const double t0 = secs();
    flush_pending();
    // Same circuit/size rules as ps2::gs::display, but the readback is asynchronous: this returns
    // the newest finished frame (usually the previous vblank's) and never waits for the GPU.
    const int circuit = (pmode & 2u) ? 1 : (pmode & 1u) ? 0 : -1;
    if (circuit < 0) return 0;
    const std::uint64_t fb = dispfb[circuit], disp = display[circuit];
    const std::uint32_t magh = (static_cast<std::uint32_t>(disp >> 23) & 0xFu) + 1, magv = (static_cast<std::uint32_t>(disp >> 27) & 3u) + 1;
    const std::uint32_t sw = ((static_cast<std::uint32_t>(disp >> 32) & 0xFFFu) + 1) / magh, sh = ((static_cast<std::uint32_t>(disp >> 44) & 0x7FFu) + 1) / magv;
    if (!sw || !sh || sw > 2048 || sh > 2048) return 0;
    const ps2::gs::gpu::DisplaySource src{(static_cast<std::uint32_t>(fb) & 0x1FFu) * 32, static_cast<std::uint32_t>(fb >> 9) & 0x3Fu,
                                          static_cast<std::uint32_t>(fb >> 15) & 0x1Fu, static_cast<std::uint32_t>(fb >> 32) & 0x7FFu,
                                          static_cast<std::uint32_t>(fb >> 43) & 0x7FFu, sw, sh, (smode2 & 3u) == 3u};
    std::uint32_t width = 0, height = 0;
    static const bool sync = std::getenv("HP_GSSYNC") != nullptr;   // exact frame, waits for the GPU
    const bool ok = sync ? ps2::gs::gpu::display(src, g_rgba, width, height) : ps2::gs::gpu::display_async(src, g_rgba, width, height);
    g_display_s += secs() - t0;
    if (!ok || !width || !height || static_cast<std::size_t>(width) * height > max_pixels || g_rgba.size() < static_cast<std::size_t>(width) * height)
        return 0;
    std::memcpy(out, g_rgba.data(), static_cast<std::size_t>(width) * height * 4);
    *w = static_cast<int>(width);
    *h = static_cast<int>(height);
    return 1;
}

/* Seconds spent in GIF processing, display readback and downloads since start. */
void gsb_times(double *gif, double *display, double *download) {
    std::lock_guard<std::mutex> lk(g_lock);
    {
        using namespace ps2::gs::gpu;
        static std::uint64_t lc, lb, le, lp;
        std::fprintf(stderr, "[gpu] image_data %llu calls %llu KB, emits %llu, pre ops %llu (max %llu)\n",
                     (unsigned long long)(g_hp_image_calls - lc), (unsigned long long)((g_hp_image_bytes - lb) >> 10),
                     (unsigned long long)(g_hp_emits - le), (unsigned long long)(g_hp_pre_total - lp), (unsigned long long)g_hp_pre_max);
        lc = g_hp_image_calls; lb = g_hp_image_bytes; le = g_hp_emits; lp = g_hp_pre_total;
    }
    *gif = g_gif_s;
    *display = g_display_s;
    *download = g_download_s;
}

void gsb_stats(void) {
    std::lock_guard<std::mutex> lk(g_lock);
    ps2::gs::stats();
}

}
