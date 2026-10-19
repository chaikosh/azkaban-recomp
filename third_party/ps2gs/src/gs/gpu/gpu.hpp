#pragma once

#include "../gs.hpp"

#include <cstdint>
#include <vector>

namespace ps2::gs::gpu {

enum class Mode { off, mirror, primary };

struct DisplaySource {
    std::uint32_t fbp, fbw, psm, dbx, dby, width, height;
    bool halved;
    // hp: PCRTC merge, a second circuit under this one: out = this * ALP/255 + second * (1 - ALP/255),
    // the second circuit's picture placed at (ox, oy) in this one's pixels
    std::uint32_t merge = 0;   // ALP + 1; 0 = off
    std::uint32_t fbp2 = 0, fbw2 = 0, psm2 = 0, dbx2 = 0, dby2 = 0, width2 = 0, height2 = 0;
    std::int32_t ox = 0, oy = 0;
};

// hp: the PCRTC merge rule: both read circuits on, PMODE.MMOD = 1 (fixed ALP) and SLBG = 0 blend
// circuit 1 over circuit 2.  Returns ALP + 1, or 0 when only one circuit is shown.
inline std::uint32_t pcrtc_merge(std::uint64_t pmode) {
    if ((pmode & 3u) != 3u || !((pmode >> 5) & 1u) || ((pmode >> 7) & 1u)) return 0;
    return (static_cast<std::uint32_t>(pmode >> 8) & 0xFFu) + 1;
}

// hp: what the PCRTC shows (gs_bridge.cpp, tools/gsreplay.cpp): circuit 2 when it is on, else
// circuit 1; with merge (pcrtc_merge) circuit 1's rectangle with circuit 2 blended in.
inline bool display_source(std::uint64_t pmode, std::uint64_t smode2, const std::uint64_t dispfb[2], const std::uint64_t display[2], std::uint32_t merge,
                           DisplaySource &src) {
    const auto geometry = [](std::uint64_t d, std::uint32_t &w, std::uint32_t &h, int &mh, int &mv) {
        mh = static_cast<int>((d >> 23) & 0xFu) + 1;
        mv = static_cast<int>((d >> 27) & 3u) + 1;
        w = ((static_cast<std::uint32_t>(d >> 32) & 0xFFFu) + 1) / static_cast<std::uint32_t>(mh);
        h = ((static_cast<std::uint32_t>(d >> 44) & 0x7FFu) + 1) / static_cast<std::uint32_t>(mv);
    };
    const auto buffer = [](std::uint64_t fb, std::uint32_t &fbp, std::uint32_t &fbw, std::uint32_t &psm, std::uint32_t &dbx, std::uint32_t &dby) {
        fbp = (static_cast<std::uint32_t>(fb) & 0x1FFu) * 32;
        fbw = static_cast<std::uint32_t>(fb >> 9) & 0x3Fu;
        psm = static_cast<std::uint32_t>(fb >> 15) & 0x1Fu;
        dbx = static_cast<std::uint32_t>(fb >> 32) & 0x7FFu;
        dby = static_cast<std::uint32_t>(fb >> 43) & 0x7FFu;
    };
    const int circuit = merge ? 0 : (pmode & 2u) ? 1 : (pmode & 1u) ? 0 : -1;
    if (circuit < 0) return false;
    std::uint32_t w, h;
    int mh, mv;
    geometry(display[circuit], w, h, mh, mv);
    if (!w || !h || w > 2048 || h > 2048) return false;
    src = DisplaySource{};
    buffer(dispfb[circuit], src.fbp, src.fbw, src.psm, src.dbx, src.dby);
    src.width = w;
    src.height = h;
    src.halved = (smode2 & 3u) == 3u;
    if (merge) {
        int mh2, mv2;
        geometry(display[1], src.width2, src.height2, mh2, mv2);
        buffer(dispfb[1], src.fbp2, src.fbw2, src.psm2, src.dbx2, src.dby2);
        src.merge = merge;
        src.ox = (static_cast<int>(display[1] & 0xFFFu) - static_cast<int>(display[0] & 0xFFFu)) / mh2;
        src.oy = (static_cast<int>((display[1] >> 12) & 0x7FFu) - static_cast<int>((display[0] >> 12) & 0x7FFu)) / mv2;
    }
    return true;
}

struct SurfaceTarget {
    enum Kind { X11, Wayland, Canvas } kind;
    void *display;
    std::uint64_t window;
    void *surface;
    const char *selector;
    std::uint32_t width, height;
};

Mode mode();
bool init(Mode m);
void draw(const State &s, const Vertex *v, int count);
void image_begin(const State &s);
void image_data(const std::uint8_t *data, std::size_t bytes);
void local_copy(const State &s);
void load_clut(std::uint64_t tex0, std::uint64_t texclut);
void flush();
void texflush();
void wait_idle();
void download(std::uint8_t *memory, std::uint32_t offset = 0, std::uint32_t bytes = kMemorySize);
bool download_async(std::uint8_t *memory, std::uint32_t offset, std::uint32_t bytes, std::uint32_t serial);
bool finish_download(std::uint32_t serial);
void read_state(std::vector<std::uint32_t> &vram, std::uint32_t &scale, std::vector<std::uint32_t> &clut);
void write_state(const std::vector<std::uint32_t> &vram, std::uint32_t scale, const std::vector<std::uint32_t> &clut);
bool display(const DisplaySource &src, std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height);
bool display_async(const DisplaySource &src, std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height);
// hp: the newest readback copy that finished since the last frame returned, without starting one
bool poll_display(std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height);
bool attach_surface(const SurfaceTarget &target);
void resize_surface(std::uint32_t width, std::uint32_t height);
bool has_surface();
bool present(const DisplaySource &src);
double busy_ms();
void upload_report();   // hp
void stats();

}
