// Replays a GS trace recorded with HP_GSREC=file (runtime/gs_bridge.cpp) through the GPU renderer:
//   gsreplay trace [outdir [first last]]
// Every frame request ('S') is read back exactly (synchronously); frames numbered first..last are
// written to outdir as frame_NNNNN.ppm.  Prints the time spent, so renderer changes can be
// compared frame for frame and timed on the same input.  The renderer's own switches (PS2_SCALE,
// HP_GPU*, PS2_GPU_PROFILE...) apply as in the game; HP_REPLAYASYNC=1 reads frames back
// asynchronously like the game (for timing; the frames written then lag).
#include "gs/gif.hpp"
#include "gs/gs.hpp"
#include "gs/gpu/gpu.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace ps2 {
static std::uint64_t g_frames = 0;
std::uint64_t frame_count() { return g_frames; }
}

namespace {
struct Show { std::uint64_t pmode, smode2, dispfb[2], display[2]; };

bool source_of(const Show &r, ps2::gs::gpu::DisplaySource &src) {   // as in gs_bridge.cpp
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

double secs() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: gsreplay trace [outdir [first last]]\n");
        return 2;
    }
    const char *outdir = argc > 2 ? argv[2] : nullptr;
    const long first = argc > 4 ? std::atol(argv[3]) : 0, last = argc > 4 ? std::atol(argv[4]) : (1L << 40);
    FILE *f = std::fopen(argv[1], "rb");
    char magic[8];
    if (!f || std::fread(magic, 1, 8, f) != 8 || std::memcmp(magic, "HPGSREC1", 8)) {
        std::fprintf(stderr, "gsreplay: %s is not a GS trace\n", argv[1]);
        return 1;
    }
    ps2::gs::init();
    if (ps2::gs::gpu::mode() != ps2::gs::gpu::Mode::primary) {
        std::fprintf(stderr, "gsreplay: WebGPU unavailable\n");
        return 1;
    }
    ps2::gs::reset();
    ps2::gif::reset();
    std::vector<std::uint8_t> data, down;
    std::vector<std::uint32_t> rgba;
    long frames = 0, written = 0;
    std::uint64_t bytes = 0;
    double t_show = 0;
    const double t0 = secs();
    for (int tag; (tag = std::fgetc(f)) != EOF;) {
        if (tag == 'G') {
            std::uint32_t hdr[2];
            if (std::fread(hdr, 4, 2, f) != 2) break;
            data.resize(static_cast<std::size_t>(hdr[1]) * 16);
            if (std::fread(data.data(), 1, data.size(), f) != data.size()) break;
            ps2::gif::render(static_cast<int>(hdr[0]) + 1, data.data(), hdr[1]);
            bytes += data.size();
        } else if (tag == 'S') {
            Show r;
            if (std::fread(&r, sizeof r, 1, f) != 1) break;
            const long n = frames++;
            ++ps2::g_frames;
            // HP_REPLAYPACE=fps: frame requests no faster than the game makes them
            static const double pace = std::getenv("HP_REPLAYPACE") ? std::atof(std::getenv("HP_REPLAYPACE")) : 0.0;
            if (pace > 0) {
                static const double start = secs();
                const double due = start + static_cast<double>(n) / pace;
                while (secs() < due) std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
            static const bool every100 = std::getenv("HP_REPLAYSTEPS") != nullptr;
            static double t_last = secs();
            if (every100 && n % 100 == 0 && n) {
                static double b_last = 0;
                const double b = ps2::gs::gpu::busy_ms();
                std::fprintf(stderr, "gsreplay: frames %ld-%ld %.2f ms/frame, GPU passes %.2f ms/frame\n", n - 100, n, (secs() - t_last) * 10, (b - b_last) / 100);
                t_last = secs();
                b_last = b;
            }
            // HP_REPLAYSHOWEVERY=n: run only every n-th frame request (what fewer requests would cost)
            static const long show_every = std::getenv("HP_REPLAYSHOWEVERY") ? std::atol(std::getenv("HP_REPLAYSHOWEVERY")) : 1;
            if (show_every > 1 && n % show_every) continue;
            ps2::gs::gpu::DisplaySource src;
            if (!source_of(r, src)) continue;
            const double ts = secs();
            std::uint32_t w = 0, h = 0;
            // HP_REPLAYASYNC=1: read frames back the way the game does (the newest finished copy),
            // which times the GS thread and the GPU overlapped instead of frame by frame
            static const bool async = std::getenv("HP_REPLAYASYNC") != nullptr;
            if (!(async ? ps2::gs::gpu::display_async(src, rgba, w, h) : ps2::gs::gpu::display(src, rgba, w, h))) continue;
            t_show += secs() - ts;
            if (outdir && n >= first && n <= last && w && h) {
                char path[1200];
                std::snprintf(path, sizeof path, "%s/frame_%05ld.ppm", outdir, n);
                if (FILE *o = std::fopen(path, "wb")) {
                    std::fprintf(o, "P6\n%u %u\n255\n", w, h);
                    std::vector<std::uint8_t> row(static_cast<std::size_t>(w) * 3);
                    for (std::uint32_t y = 0; y < h; ++y) {
                        for (std::uint32_t x = 0; x < w; ++x) {
                            const std::uint32_t c = rgba[static_cast<std::size_t>(y) * w + x];
                            row[x * 3] = static_cast<std::uint8_t>(c);
                            row[x * 3 + 1] = static_cast<std::uint8_t>(c >> 8);
                            row[x * 3 + 2] = static_cast<std::uint8_t>(c >> 16);
                        }
                        std::fwrite(row.data(), 1, row.size(), o);
                    }
                    std::fclose(o);
                    ++written;
                }
            }
        } else if (tag == 'R') {
            std::uint32_t q;
            if (std::fread(&q, 4, 1, f) != 1) break;
            down.resize(static_cast<std::size_t>(q) * 16);
            ps2::gs::read_image(down.data(), q);
        } else {
            std::fprintf(stderr, "gsreplay: bad record %d at %ld\n", tag, std::ftell(f));
            return 1;
        }
    }
    const double t = secs() - t0;
    std::fprintf(stderr, "gsreplay: GS passes on the GPU %.0f ms\n", ps2::gs::gpu::busy_ms());
    std::fprintf(stderr, "gsreplay: %ld frames (%ld written), %.1f MB GIF, %.2f s (%.2f ms/frame, readback %.2f s)\n", frames, written,
                 static_cast<double>(bytes) / (1 << 20), t, frames ? t * 1000 / static_cast<double>(frames) : 0.0, t_show);
    return 0;
}
