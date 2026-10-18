/* HLE implementations of the IOP RPC servers the game binds. */
#include "rt.h"
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>

typedef int (*RpcFn)(uint32_t, uint8_t *, uint32_t, uint8_t *, uint32_t, uint32_t *);
void iop_register(uint32_t sid, const char *name, RpcFn fn);
extern uint8_t *g_iop_ram;

static uint32_t in32(const uint8_t *p, int k) { uint32_t v; memcpy(&v, p + 4 * k, 4); return v; }
static void out32(uint8_t *p, int k, uint32_t v) { memcpy(p + 4 * k, &v, 4); }
static void ret1(uint8_t *out, uint32_t cap, uint32_t *len, uint32_t v) {
    memset(out, 0, cap);
    if (cap >= 4) out32(out, 0, v);
    *len = cap;
}

/* ---- disc image ------------------------------------------------------------ */
static int iso_fd = -1;
static uint32_t iso_sectors;

static int iso_open(void) {
    if (iso_fd >= 0) return 1;
    char path[1200];
    const char *env = getenv("HP_ISO");
    if (env) snprintf(path, sizeof path, "%s", env);
    else snprintf(path, sizeof path, "%s/../iso/Harry Potter and the Prisoner of Azkaban (USA).iso", g_disc_dir);
    iso_fd = open(path, O_RDONLY);
    if (iso_fd < 0) { LOG("[cdvd] cannot open disc image %s\n", path); return 0; }
    iso_sectors = (uint32_t)(lseek(iso_fd, 0, SEEK_END) / 2048);
    LOG("[cdvd] disc image: %u sectors\n", iso_sectors);
    return 1;
}

int cdvd_read_sectors(uint32_t lsn, uint32_t count, void *dst) {
    if (!iso_open()) { memset(dst, 0, (size_t)count * 2048); return 0; }
    ssize_t n = pread(iso_fd, dst, (size_t)count * 2048, (off_t)lsn * 2048);
    if (n < (ssize_t)count * 2048) {
        size_t got = n > 0 ? (size_t)n : 0;
        memset((uint8_t *)dst + got, 0, (size_t)count * 2048 - got);
        return 0;
    }
    return 1;
}

/* ISO 9660 path lookup ("\BIG\PACKAGES.BIG;1"), case-insensitive. */
int iso_find(const char *path, uint32_t *lsn, uint32_t *size) {
    uint8_t sec[2048];
    if (!cdvd_read_sectors(16, 1, sec)) return 0;
    uint32_t dlsn, dsize;
    memcpy(&dlsn, sec + 156 + 2, 4);
    memcpy(&dsize, sec + 156 + 10, 4);
    const char *p = path;
    if (!strncmp(p, "cdrom0:", 7)) p += 7;
    while (*p == '\\' || *p == '/') p++;
    for (;;) {
        char comp[64];
        int n = 0;
        while (*p && *p != '\\' && *p != '/' && n < 63) comp[n++] = (char)toupper((unsigned char)*p++);
        comp[n] = 0;
        int last = !*p;
        while (*p == '\\' || *p == '/') p++;
        char *semi = strchr(comp, ';');
        if (semi) *semi = 0;
        int found = 0;
        for (uint32_t off = 0; off < dsize && !found; off += 2048) {
            uint8_t blk[2048];
            if (!cdvd_read_sectors(dlsn + off / 2048, 1, blk)) return 0;
            for (uint32_t k = 0; k < 2048 - 33;) {
                uint8_t len = blk[k];
                if (!len) break;
                uint8_t nlen = blk[k + 32];
                char name[64];
                int m = 0;
                for (int j = 0; j < nlen && m < 63; j++) name[m++] = (char)toupper(blk[k + 33 + j]);
                name[m] = 0;
                char *s = strchr(name, ';');
                if (s) *s = 0;
                if (nlen > 1 && !strcmp(name, comp)) {
                    memcpy(&dlsn, blk + k + 2, 4);
                    memcpy(&dsize, blk + k + 10, 4);
                    found = 1;
                    break;
                }
                k += len;
            }
        }
        if (!found) return 0;
        if (last) { *lsn = dlsn; *size = dsize; return 1; }
    }
}

/* LSN -> file name map for diagnostics (walks the ISO directory tree once). */
typedef struct { uint32_t lsn, sectors; char name[64]; } IsoFile;
static IsoFile isofiles[256];
static int nisofiles;

static void iso_scan_dir(uint32_t dlsn, uint32_t dsize, const char *prefix, int depth) {
    if (depth > 4) return;
    for (uint32_t off = 0; off < dsize; off += 2048) {
        uint8_t blk[2048];
        if (!cdvd_read_sectors(dlsn + off / 2048, 1, blk)) return;
        for (uint32_t k = 0; k < 2048 - 33;) {
            uint8_t len = blk[k];
            if (!len) break;
            uint8_t nlen = blk[k + 32], flags = blk[k + 25];
            uint32_t lsn, size;
            memcpy(&lsn, blk + k + 2, 4);
            memcpy(&size, blk + k + 10, 4);
            char name[64];
            int m = 0;
            for (int j = 0; j < nlen && m < 40; j++) name[m++] = (char)blk[k + 33 + j];
            name[m] = 0;
            if (nlen > 1) {
                char full[64];
                snprintf(full, sizeof full, "%s\\%s", prefix, name);
                if (flags & 2) iso_scan_dir(lsn, size, full, depth + 1);
                else if (nisofiles < 256) isofiles[nisofiles++] = (IsoFile){lsn, (size + 2047) / 2048, ""}, snprintf(isofiles[nisofiles - 1].name, 64, "%s", full);
            }
            k += len;
        }
    }
}

const char *iso_name_of(uint32_t lsn, uint32_t *offset) {
    if (!nisofiles) {
        uint8_t sec[2048];
        if (cdvd_read_sectors(16, 1, sec)) {
            uint32_t dlsn, dsize;
            memcpy(&dlsn, sec + 156 + 2, 4);
            memcpy(&dsize, sec + 156 + 10, 4);
            iso_scan_dir(dlsn, dsize, "", 0);
        }
    }
    for (int k = 0; k < nisofiles; k++)
        if (lsn >= isofiles[k].lsn && lsn < isofiles[k].lsn + isofiles[k].sectors) {
            if (offset) *offset = (lsn - isofiles[k].lsn) * 2048;
            return isofiles[k].name;
        }
    return "?";
}

/* ---- CDVD servers --------------------------------------------------------- */
static int cd_init(uint32_t fno, uint8_t *in, uint32_t n, uint8_t *out, uint32_t cap, uint32_t *len) {
    iso_open();
    ret1(out, cap, len, 1);
    return 0;
}

static int cd_scmd(uint32_t fno, uint8_t *in, uint32_t n, uint8_t *out, uint32_t cap, uint32_t *len) {
    switch (fno) {
    case 0x01: {   /* ReadClock: result, then BCD time */
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        memset(out, 0, cap);
        if (cap >= 4) out32(out, 0, 1);
#define BCD(x) (uint8_t)((((x) / 10) << 4) | ((x) % 10))
        if (cap >= 12) {
            out[4] = 0; out[5] = BCD(tm.tm_sec); out[6] = BCD(tm.tm_min); out[7] = BCD(tm.tm_hour);
            out[8] = 0; out[9] = BCD(tm.tm_mday); out[10] = BCD(tm.tm_mon + 1); out[11] = BCD(tm.tm_year % 100);
        }
#undef BCD
        *len = cap;
        return 0;
    }
    case 0x03: ret1(out, cap, len, 0x14); return 0;   /* GetDiskType: PS2 DVD */
    case 0x04: ret1(out, cap, len, 0); return 0;      /* GetError: none */
    case 0x05: ret1(out, cap, len, 1); return 0;      /* TrayReq */
    case 0x22: ret1(out, cap, len, 1); return 0;      /* Mmode */
    }
    LOG("[cdvd] scmd %#x (in %u bytes: %08x %08x)\n", fno, n, n >= 4 ? in32(in, 0) : 0, n >= 8 ? in32(in, 1) : 0);
    ret1(out, cap, len, 1);
    return 0;
}

static int cd_diskready(uint32_t fno, uint8_t *in, uint32_t n, uint8_t *out, uint32_t cap, uint32_t *len) {
    ret1(out, cap, len, 2);   /* SCECdComplete */
    return 0;
}

static int cd_ncmd(uint32_t fno, uint8_t *in, uint32_t n, uint8_t *out, uint32_t cap, uint32_t *len) {
    switch (fno) {
    case 0x01: {   /* Read(lsn, sectors, ee buffer, mode) */
        uint32_t lsn = in32(in, 0), cnt = in32(in, 1), buf = in32(in, 2) & 0x1fffffffu;
        if (g_trace) {
            uint32_t off = 0;
            const char *nm = iso_name_of(lsn, &off);
            struct timespec tq; clock_gettime(CLOCK_MONOTONIC, &tq);
            LOG("[cdvd %.4f] read lsn %u x%u -> %08x (%s +%#x)\n", tq.tv_sec + tq.tv_nsec * 1e-9, lsn, cnt, buf, nm, off);
        }
        if (buf + cnt * 2048 <= RAM_SIZE) cdvd_read_sectors(lsn, cnt, g_ram + buf);
        else LOG("[cdvd] read to bad buffer %08x\n", buf);
        ret1(out, cap, len, 1);
        return 0;
    }
    }
    LOG("[cdvd] ncmd %#x (in %u bytes: %08x %08x %08x %08x)\n", fno, n, in32(in, 0), in32(in, 1), in32(in, 2), in32(in, 3));
    ret1(out, cap, len, 1);
    return 0;
}

static int cd_search(uint32_t fno, uint8_t *in, uint32_t n, uint8_t *out, uint32_t cap, uint32_t *len) {
    /* { sceCdlFILE file (32 bytes); char name[256]; void *ee_dest } */
    char name[257];
    memcpy(name, in + 32, 256);
    name[256] = 0;
    uint32_t dest = n >= 292 ? in32(in, 72) : 0;
    uint32_t lsn = 0, size = 0;
    int ok = iso_find(name, &lsn, &size);
    if (g_trace || !ok) LOG("[cdvd] search %s -> %s lsn %u size %u (dest %08x, in %u)\n", name, ok ? "ok" : "MISSING", lsn, size, dest, n);
    if (ok && dest) {
        uint8_t f[32];
        memset(f, 0, sizeof f);
        out32(f, 0, lsn);
        out32(f, 1, size);
        const char *base = strrchr(name, '\\');
        strncpy((char *)f + 8, base ? base + 1 : name, 15);
        memcpy(gptr(dest & 0x1fffffffu), f, 32);
    }
    ret1(out, cap, len, ok ? 1 : 0);
    return 0;
}

/* ---- module loader and IOP heap ------------------------------------------ */
static int next_mod_id = 0x20;
static char loaded_mods[32][64];
static int nloaded;
static uint32_t iop_heap_top = 0x00100000u;

static int loadfile(uint32_t fno, uint8_t *in, uint32_t n, uint8_t *out, uint32_t cap, uint32_t *len) {
    switch (fno) {
    case 0: case 1: case 4: case 5: {   /* module / ELF load: {arg_len, modres, path[252], args[252]} */
        char path[253];
        memcpy(path, in + 8, 252);
        path[252] = 0;
        LOG("[iop] load module %s\n", path);
        if (nloaded < 32) {
            const char *b = strrchr(path, '\\');
            snprintf(loaded_mods[nloaded++], 64, "%s", b ? b + 1 : path);
        }
        memset(out, 0, cap);
        if (cap >= 4) out32(out, 0, (uint32_t)next_mod_id++);
        *len = cap;
        return 0;
    }
    case 6:   /* load from a buffer in IOP memory */
        LOG("[iop] load module from IOP buffer %08x\n", in32(in, 0));
        ret1(out, cap, len, (uint32_t)next_mod_id++);
        return 0;
    }
    if (fno == 255) {   /* module version: must match the EE library (SDK 2.7.1) */
        memset(out, 0, cap);
        memcpy(out, "2710", cap < 4 ? cap : 4);
        *len = cap;
        return 0;
    }
    if (fno == 9) {   /* search module by name */
        char name[64];
        memcpy(name, in + 8, 63);
        name[63] = 0;
        int id = -1;
        for (int k = 0; k < nloaded; k++)
            if (strcasestr(loaded_mods[k], name)) id = 0x20 + k;
        if (g_trace) LOG("[iop] search module %s -> %d\n", name, id);
        ret1(out, cap, len, (uint32_t)id);
        return 0;
    }
    LOG("[iop] loadfile fn %u\n", fno);
    ret1(out, cap, len, 0);
    return 0;
}

static int iopheap(uint32_t fno, uint8_t *in, uint32_t n, uint8_t *out, uint32_t cap, uint32_t *len) {
    switch (fno) {
    case 1: {   /* alloc(size) */
        uint32_t size = (in32(in, 0) + 63) & ~63u;
        uint32_t a = iop_heap_top;
        if (a + size > 0x001F0000u) { LOG("[iop] heap exhausted (%u)\n", size); a = 0; }
        else iop_heap_top += size;
        if (g_trace) LOG("[iop] heap alloc %u -> %08x\n", size, a);
        ret1(out, cap, len, a);
        return 0;
    }
    case 2: ret1(out, cap, len, 0); return 0;   /* free */
    case 3: {   /* load a file into IOP memory: {addr, path} */
        char path[253];
        memcpy(path, in + 4, 252);
        path[252] = 0;
        LOG("[iop] heap load %s -> %08x\n", path, in32(in, 0));
        ret1(out, cap, len, 0);
        return 0;
    }
    }
    LOG("[iop] heap fn %u\n", fno);
    ret1(out, cap, len, 0);
    return 0;
}

/* ---- pads (padman.irx) and multitap (mtapman.irx) -------------------------- */
/* The IOP writes two 128-byte pad_data records per opened port into an EE
 * buffer every frame; libpad picks the one with the newer frame counter. */
typedef struct { uint32_t area; int open; uint32_t frame; } PadPort;
static PadPort pads[2][4];
static volatile uint16_t pad_buttons = 0xffff;    /* active low, DualShock layout */
static volatile uint8_t pad_axes[4] = {0x80, 0x80, 0x80, 0x80};   /* rx, ry, lx, ly */

void pad_set_state(uint16_t buttons_active_low, const uint8_t axes[4]) {
    pad_buttons = buttons_active_low;
    for (int k = 0; k < 4; k++) pad_axes[k] = axes[k];
}

static void pad_write(PadPort *pp) {
    uint8_t rec[128];
    memset(rec, 0, sizeof rec);
    pp->frame++;
    rec[0] = 0;          /* ok */
    rec[1] = 0x73;       /* DualShock, 3 halfwords of data */
    rec[2] = (uint8_t)pad_buttons; rec[3] = (uint8_t)(pad_buttons >> 8);
    rec[4] = pad_axes[0]; rec[5] = pad_axes[1]; rec[6] = pad_axes[2]; rec[7] = pad_axes[3];
    uint16_t modes[4] = {0x41, 0x73, 0, 0};
    memcpy(rec + 0x50, modes, sizeof modes);
    out32(rec, 0x58 / 4, pp->frame);
    out32(rec, 0x60 / 4, 32);           /* data length */
    rec[0x64] = 1;      /* modeConfig */
    rec[0x65] = 1;      /* current mode index: analog */
    rec[0x66] = 3;      /* model: DualShock 2 */
    rec[0x67] = 1;      /* data ready */
    rec[0x68] = 2;      /* number of modes */
    rec[0x69] = 1;      /* current mode offset */
    rec[0x6a] = 2;      /* actuators */
    rec[0x6d] = 0x73;
    rec[0x70] = 6;      /* PAD_STATE_STABLE */
    rec[0x71] = 0;      /* request state: complete */
    /* write to the buffer the EE reads next (alternate between the two) */
    uint32_t dst = (pp->area & 0x1fffffffu) + ((pp->frame & 1) ? 0 : 128);
    if (dst + 128 <= RAM_SIZE) memcpy(g_ram + dst, rec, 128);
}

void pad_vblank(void) {
    for (int p = 0; p < 2; p++)
        for (int s = 0; s < 4; s++)
            if (pads[p][s].open && pads[p][s].area) pad_write(&pads[p][s]);
}

static int pad_rpc(uint32_t fno, uint8_t *in, uint32_t n, uint8_t *out, uint32_t cap, uint32_t *len) {
    uint32_t cmd = in32(in, 0);
    memset(out, 0, cap);
    *len = cap;
    switch (cmd) {
    case 0x01: {   /* open: {cmd, port, slot, result, area} */
        uint32_t port = in32(in, 1), slot = in32(in, 2), area = in32(in, 4);
        if (port < 2 && slot < 4) {
            pads[port][slot] = (PadPort){area, port == 0 && slot == 0, 0};
            if (pads[port][slot].open) { pad_write(&pads[port][slot]); pad_write(&pads[port][slot]); }
        }
        if (g_trace) LOG("[pad] open port %u slot %u area %08x\n", port, slot, area);
        out32(out, 3, 1);
        return 0;
    }
    case 0x0e:    /* close */
        if (in32(in, 1) < 2 && in32(in, 2) < 4) pads[in32(in, 1)][in32(in, 2)].open = 0;
        out32(out, 3, 1);
        return 0;
    case 0x09: out32(out, 3, 0x3ffff); return 0;   /* button mask: all */
    case 0x0c: out32(out, 3, 2); return 0;         /* port max */
    case 0x0d: out32(out, 3, 1); return 0;         /* slot max */
    case 0x12: out32(out, 3, 1); return 0;         /* module version */
    default:
        if (g_trace) LOG("[pad] cmd %08x %08x %08x %08x\n", cmd, in32(in, 1), in32(in, 2), in32(in, 3));
        out32(out, 3, 1);
        return 0;
    }
}

static int mtap_rpc(uint32_t fno, uint8_t *in, uint32_t n, uint8_t *out, uint32_t cap, uint32_t *len) {
    /* no multitap connected: every request fails politely */
    memset(out, 0, cap);
    *len = cap;
    return 0;
}

/* ---- memory card (mcserv.irx) ---------------------------------------------- */
static int mc_rpc(uint32_t fno, uint8_t *in, uint32_t n, uint8_t *out, uint32_t cap, uint32_t *len) {
    memset(out, 0, cap);
    *len = cap;
    if (fno == 0xfe) {   /* init: result, mcserv version, mcman version */
        if (cap >= 12) { out32(out, 0, 0); out32(out, 1, 0x20b); out32(out, 2, 0x20f); }
        return 0;
    }
    if (fno == 0x01) {   /* GetInfo: {port, slot, ?, want type, want free, want format, ?, ee param} */
        uint32_t port = in32(in, 0), param = in32(in, 7) & 0x1fffffffu;
        static int seen_card[2];
        if (port == 0) {
            if (param + 148 <= RAM_SIZE) {
                uint32_t type = 2, free_kb = 8000, fmt = 1;
                memcpy(g_ram + param + 0, &type, 4);
                memcpy(g_ram + param + 4, &free_kb, 4);
                memcpy(g_ram + param + 144, &fmt, 4);
            }
            if (cap >= 4) out32(out, 0, seen_card[0]++ ? 0 : (uint32_t)-1);   /* -1 once: new formatted card */
        } else if (cap >= 4) out32(out, 0, (uint32_t)-10);   /* no card in slot 2 */
        return 0;
    }
    if (fno == 0x0c) { if (cap >= 4) out32(out, 0, 0); return 0; }   /* ChDir: ok */
    if (fno == 0x0d) { if (cap >= 4) out32(out, 0, 0); return 0; }   /* GetDir: no saves yet */
    static int seen[256];
    if (fno < 256 && seen[fno]++ < 3) {
        LOG("[mc] fn %#x (in %u):", fno, n);
        for (uint32_t k = 0; k < n / 4 && k < 10; k++) LOG(" %08x", in32(in, k));
        LOG("\n");
    }
    if (cap >= 4) out32(out, 0, (uint32_t)-1);   /* no card for now */
    return 0;
}

/* EA sound driver RPC ("SND"): logged until the protocol is understood. */
static int snd_rpc(uint32_t fno, uint8_t *in, uint32_t n, uint8_t *out, uint32_t cap, uint32_t *len) {
    static int seen[64];
    if (fno < 64 && seen[fno]++ < 3) {
        LOG("[snd] rpc fn %u send %u recv %u:", fno, n, cap);
        for (uint32_t k = 0; k < n / 4 && k < 8; k++) LOG(" %08x", in32(in, k));
        LOG("\n");
    }
    ret1(out, cap, len, 0);
    return 0;
}

void iop_servers_init(void) {
    iop_register(0x00534e44, "snd", snd_rpc);
    iop_register(0x80000100, "pad", pad_rpc);
    iop_register(0x80000400, "mcserv", mc_rpc);
    iop_register(0x80000101, "pad2", pad_rpc);
    for (uint32_t k = 0x80000901; k <= 0x80000905; k++) iop_register(k, "mtap", mtap_rpc);
    iop_register(0x80000003, "iopheap", iopheap);
    iop_register(0x80000006, "loadfile", loadfile);
    iop_register(0x80000592, "cdvd-init", cd_init);
    iop_register(0x80000593, "cdvd-scmd", cd_scmd);
    iop_register(0x80000595, "cdvd-ncmd", cd_ncmd);
    iop_register(0x80000597, "cdvd-search", cd_search);
    iop_register(0x8000059c, "cdvd-diskready", cd_diskready);
}
