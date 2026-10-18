// GPU renderer for the hp runtime (HP_GS=gpu): ps2-web-recomp's GS (vendored in third_party/ps2gs,
// MIT) draws with WebGPU compute shaders through wgpu-native (Metal on Apple platforms).
//
// hp's own GS front end (runtime/gs.c) still parses every GIF packet for the privileged side
// (SIGNAL/FINISH/LABEL, CSR, interrupts) but stops rasterizing; the same packets are handed here
// in the same order.  Downloads (local->host transfers) and the displayed frame come back from the
// GPU.
//
// Like the real GS, the renderer runs beside the rest of the machine: GIF data is gathered into
// batches (runs per path, in arrival order) and a GS thread parses and draws them; showing a
// frame is a request in the same queue, and the vblank thread picks up the newest finished frame.
// Only a download (local->host transfer) waits for the queue to drain.  The vendored GS is
// single-threaded: everything that touches it holds g_lock.  HP_GSTHREAD=0 renders on the calling
// threads instead.

#include "gs/gif.hpp"
#include "gs/gs.hpp"
#include "gs/gpu/gpu.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" unsigned long long g_vsyncs;
extern "C" void hp_thread_register(const char *name);
namespace ps2 {
std::uint64_t frame_count() { return g_vsyncs; }
}

namespace ps2::gs::gpu {
extern std::uint64_t g_hp_image_calls, g_hp_image_bytes, g_hp_emits, g_hp_pre_total, g_hp_pre_max;
}

extern "C" int gsb_mirror(void);

namespace {
std::mutex g_lock;   // the vendored GS
bool g_ready = false;
double g_gif_s = 0, g_display_s = 0, g_download_s = 0;

double secs() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct Run { int path; std::uint32_t qwc; };
struct Batch { std::vector<std::uint8_t> data; std::vector<Run> runs; };

// What a frame shows (from the privileged registers).
struct Show { std::uint64_t pmode, smode2, dispfb[2], display[2]; };
struct Item { Batch batch; bool show = false; Show regs{}; };

// --- gathering (any thread) ---
std::mutex g_pend_lock;
Batch g_pend;
bool g_new_since_show = false;   // a batch was posted since the last frame request (g_pend_lock)

// --- the queue to the GS thread ---
std::mutex g_q_lock;
std::condition_variable g_q_cv, g_q_done_cv;
std::deque<Item> g_queue;
std::size_t g_queued_bytes = 0;
int g_shows_queued = 0;   // frame requests not yet run by the GS thread (g_q_lock)
bool g_gs_busy = false, g_threaded = false;
std::vector<Batch> g_free;   // rendered batches with their capacity kept (g_q_lock)

// --- the newest finished frame ---
std::mutex g_frame_lock;
std::vector<std::uint32_t> g_frame;
std::uint32_t g_frame_w = 0, g_frame_h = 0;
std::uint64_t g_frame_serial = 0;

// HP_GSREC=file records everything the vendored GS is given, in order, for tools/gsreplay
// (renderer changes compared frame for frame on the same input, and GPU time measured without the
// game): 'G' path qwc data | 'S' Show | 'R' qwords.  HP_GSRECMAX=MB stops it (default 4000).
FILE *rec_file() {   // g_lock held
    static FILE *f = nullptr;
    static bool tried = false;
    static std::uint64_t left = 0;
    if (!tried) {
        tried = true;
        if (const char *p = std::getenv("HP_GSREC")) {
            f = std::fopen(p, "wb");
            if (f) {
                static char buf[4 << 20];
                std::setvbuf(f, buf, _IOFBF, sizeof buf);
                std::fwrite("HPGSREC1", 1, 8, f);
            }
            left = static_cast<std::uint64_t>(std::getenv("HP_GSRECMAX") ? std::atoi(std::getenv("HP_GSRECMAX")) : 4000) << 20;
        }
    }
    if (f && static_cast<std::uint64_t>(std::ftell(f)) > left) {
        std::fclose(f);
        f = nullptr;
        std::fprintf(stderr, "[gsrec] size limit reached, recording stopped\n");
    }
    return f;
}
void rec_put(char tag, const void *a, std::size_t na, const void *b = nullptr, std::size_t nb = 0) {   // g_lock held
    FILE *f = rec_file();
    if (!f) return;
    std::fputc(tag, f);
    std::fwrite(a, 1, na, f);
    if (nb) std::fwrite(b, 1, nb, f);
}

void render_batch(Batch &b) {   // g_lock held
    const double t0 = secs();
    std::size_t off = 0;
    for (const Run &r : b.runs) {
        if (rec_file()) {
            const std::uint32_t hdr[2] = {static_cast<std::uint32_t>(r.path), r.qwc};
            rec_put('G', hdr, sizeof hdr, b.data.data() + off, static_cast<std::size_t>(r.qwc) * 16);
        }
        ps2::gif::render(r.path + 1, b.data.data() + off, r.qwc);
        off += static_cast<std::size_t>(r.qwc) * 16;
    }
    b.data.clear();
    b.runs.clear();
    g_gif_s += secs() - t0;
}

bool source_of(const Show &r, ps2::gs::gpu::DisplaySource &src) {
    const int circuit = (r.pmode & 2u) ? 1 : (r.pmode & 1u) ? 0 : -1;
    if (circuit < 0) return false;
    const std::uint64_t fb = r.dispfb[circuit], disp = r.display[circuit];
    const std::uint32_t magh = (static_cast<std::uint32_t>(disp >> 23) & 0xFu) + 1, magv = (static_cast<std::uint32_t>(disp >> 27) & 3u) + 1;
    const std::uint32_t sw = ((static_cast<std::uint32_t>(disp >> 32) & 0xFFFu) + 1) / magh, sh = ((static_cast<std::uint32_t>(disp >> 44) & 0x7FFu) + 1) / magv;
    if (!sw || !sh || sw > 2048 || sh > 2048) return false;
    src = ps2::gs::gpu::DisplaySource{(static_cast<std::uint32_t>(fb) & 0x1FFu) * 32, static_cast<std::uint32_t>(fb >> 9) & 0x3Fu,
                                      static_cast<std::uint32_t>(fb >> 15) & 0x1Fu, static_cast<std::uint32_t>(fb >> 32) & 0x7FFu,
                                      static_cast<std::uint32_t>(fb >> 43) & 0x7FFu, sw, sh, (r.smode2 & 3u) == 3u};
    return true;
}

void publish(const std::vector<std::uint32_t> &rgba, std::uint32_t w, std::uint32_t h) {
    if (!w || !h || rgba.size() < static_cast<std::size_t>(w) * h) return;
    std::lock_guard<std::mutex> fl(g_frame_lock);
    g_frame.assign(rgba.begin(), rgba.begin() + static_cast<std::ptrdiff_t>(w) * h);
    g_frame_w = w;
    g_frame_h = h;
    ++g_frame_serial;
}

// Reads back the displayed buffer (asynchronously: the newest finished copy) into g_frame.
unsigned long long g_shows_done = 0;   // frame requests run by the GS thread (for the [fps] line)

void show_frame(const Show &r) {   // g_lock held
    const double t0 = secs();
    ++g_shows_done;
    rec_put('S', &r, sizeof r);
    ps2::gs::gpu::DisplaySource src;
    if (source_of(r, src)) {
        static std::vector<std::uint32_t> rgba;
        std::uint32_t w = 0, h = 0;
        // The asynchronous readback keeps up to three copies in flight and waits for the oldest when
        // all are busy (gpu.cpp), which bounds the GPU queue; HP_GSSYNC=1 reads every frame exactly
        // (the GS thread then waits for the GPU each time: slower), as does HP_GS=both.
        static const bool sync = std::getenv("HP_GSSYNC") != nullptr || gsb_mirror();
        const bool ok = sync ? ps2::gs::gpu::display(src, rgba, w, h) : ps2::gs::gpu::display_async(src, rgba, w, h);
        if (ok) publish(rgba, w, h);
    }
    g_display_s += secs() - t0;
}

// A readback that finished since the last frame request is handed on now (the GS thread checks
// after each batch and every 2 ms while idle): the vblank that showed a frame used to depend on
// when the next request reached the GS thread.  HP_FRAMEPOLL=0 waits for the request again.
void poll_frame() {   // g_lock held
    static const bool on = !std::getenv("HP_FRAMEPOLL") || std::atoi(std::getenv("HP_FRAMEPOLL")) != 0;
    if (!on) return;
    static std::vector<std::uint32_t> rgba;
    std::uint32_t w = 0, h = 0;
    if (ps2::gs::gpu::poll_display(rgba, w, h)) publish(rgba, w, h);
}

void gs_thread() {
    hp_thread_register("gs");
    std::unique_lock<std::mutex> ql(g_q_lock);
    for (;;) {
        while (!g_q_cv.wait_for(ql, std::chrono::milliseconds(2), [] { return !g_queue.empty(); })) {
            ql.unlock();
            {
                std::lock_guard<std::mutex> lk(g_lock);
                poll_frame();
            }
            ql.lock();
        }
        Item it = std::move(g_queue.front());
        g_queue.pop_front();
        g_queued_bytes -= it.batch.data.size();
        if (it.show) --g_shows_queued;
        g_gs_busy = true;
        ql.unlock();
        {
            std::lock_guard<std::mutex> lk(g_lock);
            render_batch(it.batch);
            if (it.show) show_frame(it.regs);
            else poll_frame();
        }
        ql.lock();
        if (it.batch.data.capacity() && g_free.size() < 8) g_free.push_back(std::move(it.batch));
        g_gs_busy = false;
        g_q_done_cv.notify_all();
    }
}

// Hands the gathered data (and optionally a frame request) on; the caller holds g_pend_lock,
// which keeps the batches in arrival order.
bool queue_full() {   // g_pend_lock held; checks the GS thread's backlog without waiting
    std::lock_guard<std::mutex> ql(g_q_lock);
    return g_threaded && g_queued_bytes >= (4u << 20);
}

void post_locked(bool show, const Show *regs) {
    if (!g_threaded) {
        std::lock_guard<std::mutex> lk(g_lock);
        render_batch(g_pend);   // clears it, keeping the capacity
        if (show) show_frame(*regs);
        return;
    }
    if (g_pend.data.empty() && !show) return;
    Item it;
    if (show) { it.show = true; it.regs = *regs; }
    std::unique_lock<std::mutex> ql(g_q_lock);
    // Latency: the GS may run at most ~2 frames behind (the controls felt late with 16 MB of
    // queued GIF data), and a frame request waits while an earlier one is still queued.
    if (it.show && g_shows_queued > 0) {
        it.show = false;
        if (g_pend.data.empty()) return;
    }
    std::swap(it.batch, g_pend);
    if (!g_free.empty()) {   // gather the next batch into a buffer that is already mapped
        g_pend = std::move(g_free.back());
        g_free.pop_back();
    }
    // a GS far behind holds up the producers, like a full GIF FIFO
    g_q_done_cv.wait(ql, [] { return g_queued_bytes < (4u << 20); });
    g_queued_bytes += it.batch.data.size();
    if (it.show) ++g_shows_queued;
    g_queue.push_back(std::move(it));
    g_q_cv.notify_one();
}

void wait_idle() {
    if (!g_threaded) return;
    std::unique_lock<std::mutex> ql(g_q_lock);
    g_q_done_cv.wait(ql, [] { return g_queue.empty() && !g_gs_busy; });
}
}

extern "C" {

int gsb_enabled(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = std::getenv("HP_GS");
        on = e && (std::string(e) == "gpu" || std::string(e) == "both");
    }
    return on;
}

/* HP_GS=both: the software GS draws and is shown; the GPU renderer gets the same packets on the
   calling thread, and the screenshots (HP_SHOT) also save its exact frame, for comparisons. */
int gsb_mirror(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = std::getenv("HP_GS");
        on = e && std::string(e) == "both";
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
    const char *t = std::getenv("HP_GSTHREAD");
    g_threaded = (!t || std::string(t) != "0") && !gsb_mirror();
    if (g_threaded) std::thread(gs_thread).detach();
    std::fprintf(stderr, "[gsgpu] GPU renderer active%s\n", g_threaded ? " (GS thread)" : "");
    return 1;
}

/* GIF data in hp's path numbering (0 = PATH1/VU1, 1 = PATH2/VIF1 DIRECT, 2 = PATH3/GIF DMA). */
void gsb_gif(int path, const void *data, uint32_t qwc) {
    if (!qwc) return;
    std::lock_guard<std::mutex> pl(g_pend_lock);
    if (!g_pend.runs.empty() && g_pend.runs.back().path == path) g_pend.runs.back().qwc += qwc;
    else g_pend.runs.push_back(Run{path, qwc});
    const auto *p = static_cast<const std::uint8_t *>(data);
    g_pend.data.insert(g_pend.data.end(), p, p + static_cast<std::size_t>(qwc) * 16);
    if (g_pend.data.size() >= (256u << 10)) { post_locked(false, nullptr); g_new_since_show = true; }
}

/* Next `qwords` of a local->host transfer started by TRXDIR=1: everything issued before it is drawn first. */
void gsb_read_image(uint8_t *dst, uint32_t qwords) {
    const double t0 = secs();
    {
        std::lock_guard<std::mutex> pl(g_pend_lock);
        post_locked(false, nullptr);
    }
    wait_idle();
    std::lock_guard<std::mutex> lk(g_lock);
    rec_put('R', &qwords, sizeof qwords);
    ps2::gs::read_image(dst, qwords);
    g_download_s += secs() - t0;
}

/* Called every vblank: asks for the displayed buffer and returns the newest finished frame as RGBA
   (R in the low byte), at most max_pixels; returns 0 if no new frame is ready. */
int gsb_display(uint64_t pmode, uint64_t smode2, const uint64_t dispfb[2], const uint64_t display[2], uint32_t *out,
                uint32_t max_pixels, int *w, int *h) {
    const Show regs{pmode, smode2, {dispfb[0], dispfb[1]}, {display[0], display[1]}};
    {
        // A new frame can only differ when GIF data arrived or the display registers changed (a
        // buffer flip): vblanks with neither are skipped.
        static Show last{};
        static bool have_last = false;
        static unsigned since = 0;
        std::lock_guard<std::mutex> pl(g_pend_lock);
        // the vblank thread never waits for the GS: it also drives the IOP's vblank interrupts and
        // the timers, and a late vblank there broke the sound on the iPhone (padman VBLANK OVERLAP)
        if (queue_full()) goto pick_up;
        const bool flip = !have_last || std::memcmp(&last, &regs, sizeof regs);
        const bool drawn = !g_pend.data.empty() || g_new_since_show;
        ++since;
        // this game does not flip DISPFB per frame, so any vblank with new data asks for a frame
        // (HP_SHOWEVERY=n: at most every n-th vblank without a flip)
        static const unsigned every = std::getenv("HP_SHOWEVERY") ? std::atoi(std::getenv("HP_SHOWEVERY")) : 1;
        if (flip || (drawn && since >= every)) {
            post_locked(true, &regs);
            last = regs;
            have_last = true;
            g_new_since_show = false;
            since = 0;
        }
    }
pick_up:
    static std::uint64_t seen = 0;
    std::lock_guard<std::mutex> fl(g_frame_lock);
    if (g_frame_serial == seen || !g_frame_w || static_cast<std::size_t>(g_frame_w) * g_frame_h > max_pixels) return 0;
    seen = g_frame_serial;
    std::memcpy(out, g_frame.data(), static_cast<std::size_t>(g_frame_w) * g_frame_h * 4);
    *w = static_cast<int>(g_frame_w);
    *h = static_cast<int>(g_frame_h);
    return 1;
}

/* iOS: hold the GS while the app is inactive (no GPU work may be submitted in the background);
   every thread that reaches the GS waits.  Called on the main thread: 1 locks, 0 unlocks. */
void gsb_pause(int on) {
    if (on) g_lock.lock();
    else g_lock.unlock();
}

/* Seconds spent in GIF processing, display readback and downloads since start. */
void gsb_times(double *gif, double *display, double *download) {
    std::lock_guard<std::mutex> lk(g_lock);
    *gif = g_gif_s;
    *display = g_display_s;
    *download = g_download_s;
}

/* GPU time of the GS's compute passes so far (ms, from timestamps on a sample of the passes);
   read without the GS lock (an estimate for the [fps] line). */
double gsb_gpu_busy_ms(void) {
    return g_ready ? ps2::gs::gpu::busy_ms() : 0.0;
}
unsigned long long gsb_shows(void) { return g_shows_done; }

void gsb_stats(void) {
    std::lock_guard<std::mutex> lk(g_lock);
    using namespace ps2::gs::gpu;
    std::fprintf(stderr, "[gpu] image_data %llu calls %llu KB, emits %llu, pre ops %llu (max %llu)\n",
                 (unsigned long long)g_hp_image_calls, (unsigned long long)(g_hp_image_bytes >> 10),
                 (unsigned long long)g_hp_emits, (unsigned long long)g_hp_pre_total, (unsigned long long)g_hp_pre_max);
    ps2::gs::stats();
}

}
