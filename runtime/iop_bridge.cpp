// Low-level IOP for the hp runtime: runs the game's own IOP modules (sound driver, disc streaming,
// pads, memory cards) on the R3000 interpreter and IOP kernel from ps2-web-recomp (vendored in
// third_party/ps2iop, MIT).  The EE side stays ours; this file adapts the two.
//
// Threading: the IOP is single-threaded.  It runs on its own host thread, catching up with the
// emulated EE clock, and every entry point from the EE side takes the same lock.

#include "iop/iop.hpp"
#include "iop/sif.hpp"
#include "iop/sio2.hpp"
#include "iop/spu2.hpp"
#include "ps2/disc.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"
#include "hw.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

extern "C" {
uint64_t virt_ns(void);
void fastboot_note(const char *event);
void kern_raise_dmac(int ch);
}

namespace ps2 {
alignas(4096) std::uint8_t g_ram[kRamSize + 0x10000];
std::uint8_t g_spr[kScratchpadSize];
Options g_options;

// EE cycles since the bridge started (virt_ns counts from host boot: scaling it directly overflows)
static std::uint64_t g_t0_ns = 0;
std::uint64_t now() { return (virt_ns() - g_t0_ns) * 294912ull / 1000000ull; }

namespace hw {
void dmac_raise(int channel) { kern_raise_dmac(channel); }
}
}

namespace {
std::recursive_mutex g_lock;
std::condition_variable_any g_wake;
std::uint64_t g_synced = 0;   // EE cycles the IOP has caught up to
bool g_running = false;
void (*g_sink)(const std::int16_t *, std::size_t) = nullptr;

void advance_locked() {
    const std::uint64_t t = ps2::now();
    if (t > g_synced) {
        // keep slices short so SIF replies are not held back by a long catch-up
        std::uint64_t left = t - g_synced;
        while (left) {
            const std::uint64_t step = left < 294912 ? left : 294912;   // 1 ms of EE time
            ps2::iop::run(step);
            left -= step;
        }
        g_synced = t;
    }
    ps2::sif::pump();
}

extern "C" void hp_thread_register(const char *name);
void iop_thread() {
    hp_thread_register("iop");
    std::unique_lock<std::recursive_mutex> lk(g_lock);
    while (g_running) {
        advance_locked();
        g_wake.wait_for(lk, std::chrono::microseconds(250));
    }
}
}

extern "C" {

uint8_t *iopb_ee_ram(void) { return ps2::g_ram; }

int iopb_enabled(void) {
    static int on = -1;
    if (on < 0) { const char *e = std::getenv("HP_IOP"); on = !e || std::string(e) != "hle"; }
    return on;
}

void iopb_init(const char *iso, const char *memcard) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    ps2::g_t0_ns = virt_ns();
    ps2::g_options.memcard_path = memcard ? memcard : "";
    ps2::g_options.on_memcard_saved = [] {   // the card file was rewritten (a save, a format)
        std::fprintf(stderr, "[mc] card written: %s\n", ps2::g_options.memcard_path.c_str());
        fastboot_note("card_written");
    };
    ps2::g_options.trace_iop_modules = std::getenv("HP_IOPTRACE") != nullptr;
    ps2::g_options.trace_iop_hle = std::getenv("HP_IOPTRACE") && std::string(std::getenv("HP_IOPTRACE")) == "2";
    if (!ps2::disc::open(iso)) std::fprintf(stderr, "[iop] cannot open disc image %s\n", iso);
    ps2::iop::set_trace(ps2::g_options.trace_iop_modules, ps2::g_options.trace_iop_hle);
    ps2::iop::reset();
    ps2::iop::spu2::set_sink([](const std::int16_t *samples, std::size_t frames) {
        if (g_sink) g_sink(samples, frames);
        static FILE *wav = nullptr;
        static std::uint32_t bytes = 0;
        static bool tried = false;
        if (!tried) {   // HP_WAV=path: record the SPU2 output (48 kHz stereo s16)
            tried = true;
            if (const char *p = std::getenv("HP_WAV")) {
                wav = std::fopen(p, "wb");
                static const unsigned char hdr[44] = {'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0,
                    0x80,0xBB,0,0,0x00,0xEE,0x02,0,4,0,16,0,'d','a','t','a',0,0,0,0};
                if (wav) std::fwrite(hdr, 1, 44, wav);
            }
        }
        if (wav && bytes < 400u << 20) {
            std::fwrite(samples, 4, frames, wav);
            bytes += static_cast<std::uint32_t>(frames * 4);
            const std::uint32_t riff = bytes + 36;
            std::fseek(wav, 4, SEEK_SET); std::fwrite(&riff, 4, 1, wav);
            std::fseek(wav, 40, SEEK_SET); std::fwrite(&bytes, 4, 1, wav);
            std::fseek(wav, 0, SEEK_END); std::fflush(wav);
        }
    });
    g_synced = ps2::now();
    g_running = true;
    std::thread(iop_thread).detach();
}

void iopb_kick(void) { g_wake.notify_one(); }

uint32_t iopb_sif_set_dma(uint32_t list, uint32_t count) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    advance_locked();
    const std::uint32_t id = ps2::sif::set_dma(list, count);
    g_wake.notify_one();
    return id;
}

int iopb_sif_dma_stat(uint32_t id) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    return ps2::sif::dma_stat(id);
}

void iopb_sif_packet_processed(void) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    ps2::sif::packet_processed();
}

uint32_t iopb_sif_get_reg(uint32_t reg) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    advance_locked();
    return ps2::sif::get_reg(reg);
}

void iopb_sif_set_reg(uint32_t reg, uint32_t v) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    ps2::sif::set_reg(reg, v);
}

void iopb_vblank(int start) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    advance_locked();
    ps2::iop::vblank(start != 0);
    // memory card writes reach the file once the card has been idle for 50 vblanks (nothing
    // called this before: saves were never written to disk)
    if (start) ps2::iop::sio2::flush_cards();
}

void iopb_set_pad(int pad, uint16_t pressed, const uint8_t axes[4]) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    ps2::iop::sio2::set_buttons(pad, pressed);
    for (int a = 0; a < 4; ++a) ps2::iop::sio2::set_axis(pad, a, axes[a]);
}

// write pending memory card changes now (before the process ends)
void iopb_mc_flush(void) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    for (int k = 0; k < 64; ++k) ps2::iop::sio2::flush_cards();   // past its 50-tick idle wait
}

void iopb_set_audio_sink(void (*sink)(const int16_t *samples, size_t frames)) { g_sink = sink; }

void iopb_dump(void) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    ps2::iop::dump();
    ps2::sif::dump();
}

}
