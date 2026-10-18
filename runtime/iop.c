/* High-level IOP: the SIF command/RPC protocol and the RPC servers the game binds.
 *
 * The EE sends command packets with sceSifSetDma into the IOP command buffer.
 * Answers are written straight into the EE receive buffer, followed by a SIF0
 * DMAC interrupt so the game's own SIF command handler processes them.
 */
#include "rt.h"
#include <stdlib.h>
#include <string.h>

#define IOP_RAM_SIZE  0x200000u
#define IOP_CMDBUF    0x00010000u   /* where the EE sends command packets */
#define IOP_RPCBUF    0x00020000u   /* RPC receive buffers, 64 KiB per server */

enum {
    CMD_CHANGE_SADDR = 0x80000000u, CMD_SET_SREG = 0x80000001u, CMD_INIT_CMD = 0x80000002u,
    CMD_RESET_CMD = 0x80000003u, CMD_RPC_END = 0x80000008u, CMD_RPC_BIND = 0x80000009u,
    CMD_RPC_CALL = 0x8000000Au, CMD_RPC_RDATA = 0x8000000Cu,
};

uint8_t *g_iop_ram;
static uint32_t ee_pktbuf;     /* EE address of its command receive buffer */
void hw_sif_set_smcom(uint32_t v);
void hw_sif_set_smflag(uint32_t bits);
void hw_sif_iop_reset(void);
void hw_sif_set_reg(uint32_t reg, uint32_t v);
void kern_raise_dmac(int ch);

typedef int (*RpcFn)(uint32_t fno, uint8_t *in, uint32_t in_size, uint8_t *out, uint32_t out_size, uint32_t *out_len);
typedef struct { uint32_t sid; const char *name; RpcFn fn; uint32_t buf; } Server;

#define MAX_SERVERS 64
static Server servers[MAX_SERVERS];
static int nservers;

/* ---- packets to the EE --------------------------------------------------- */
#define QMAX 64
static uint8_t q[QMAX][128];
static uint32_t qlen[QMAX];
static int qhead, qtail;
static pthread_mutex_t qm = PTHREAD_MUTEX_INITIALIZER;

uint32_t hw_sif_sysreg(int k);

static void deliver_next(void) {
    /* qm held; the Sony SDK publishes its receive buffer in SIF system register 1 */
    uint32_t buf = ee_pktbuf ? ee_pktbuf : hw_sif_sysreg(1);
    if (qhead == qtail || !buf) return;
    memcpy(gptr(buf), q[qhead], qlen[qhead]);
    qhead = (qhead + 1) % QMAX;
    kern_raise_dmac(5);
}

static int sif0_busy;
static void send_to_ee(const void *pkt, uint32_t len) {
    pthread_mutex_lock(&qm);
    memcpy(q[qtail], pkt, len);
    qlen[qtail] = len;
    qtail = (qtail + 1) % QMAX;
    if (!sif0_busy) { sif0_busy = 1; deliver_next(); }
    pthread_mutex_unlock(&qm);
}

/* Called by the kernel after the game's SIF0 handlers ran. */
void iop_sif0_done(void) {
    pthread_mutex_lock(&qm);
    if (qhead != qtail) deliver_next();
    else sif0_busy = 0;
    pthread_mutex_unlock(&qm);
}

static uint32_t rd(const uint8_t *p, int k) { uint32_t v; memcpy(&v, p + 4 * k, 4); return v; }
static void wr(uint8_t *p, int k, uint32_t v) { memcpy(p + 4 * k, &v, 4); }

static void rpc_end(const uint8_t *req, uint32_t cid, uint32_t server, uint32_t buff, uint32_t cbuff) {
    uint8_t p[48];
    memset(p, 0, sizeof p);
    wr(p, 0, 48);              /* psize = 48, no extra data */
    wr(p, 2, CMD_RPC_END);
    wr(p, 4, rd(req, 4));      /* rec_id */
    wr(p, 5, rd(req, 5));      /* pkt_addr */
    wr(p, 6, rd(req, 6));      /* rpc_id */
    wr(p, 7, rd(req, 7));      /* client */
    wr(p, 8, cid);
    wr(p, 9, server);
    wr(p, 10, buff);
    wr(p, 11, cbuff);
    send_to_ee(p, sizeof p);
}

static Server *find_server(uint32_t sid) {
    for (int k = 0; k < nservers; k++) if (servers[k].sid == sid) return &servers[k];
    return NULL;
}

static int rpc_unknown(uint32_t fno, uint8_t *in, uint32_t in_size, uint8_t *out, uint32_t out_size, uint32_t *out_len) {
    memset(out, 0, out_size);
    *out_len = out_size;
    return 0;
}

static Server *bind_server(uint32_t sid) {
    Server *s = find_server(sid);
    if (s) return s;
    if (nservers >= MAX_SERVERS) return NULL;
    s = &servers[nservers];
    s->sid = sid; s->name = "unknown"; s->fn = rpc_unknown;
    s->buf = IOP_RPCBUF + 0x10000u * (uint32_t)nservers;
    nservers++;
    return s;
}

void iop_register(uint32_t sid, const char *name, RpcFn fn) {
    Server *s = bind_server(sid);
    if (s) { s->name = name; s->fn = fn; }
}

/* EA SNDDRV.IRX speaks SIF command 2 (EE -> IOP) and answers with command 3. */
static void snd_reply(uint32_t type, uint32_t value) {
    uint8_t r[24];
    memset(r, 0, sizeof r);
    wr(r, 0, sizeof r);
    wr(r, 2, 3);
    wr(r, 4, type);
    wr(r, 5, value);
    send_to_ee(r, sizeof r);
}

static void snd_command(const uint8_t *p) {
    uint32_t psize = rd(p, 0) & 0xff, type = rd(p, 4);
    switch (type) {
    case 0:   /* hello: report driver version 1.05 */
        snd_reply(0, 0x105);
        return;
    }
    LOG("[snd] command type %u (psize %u):", type, psize);
    for (uint32_t k = 5; k < psize / 4 && k < 16; k++) LOG(" %08x", rd(p, k));
    LOG("\n");
}

/* EA's IOP sound module also takes SIF command 0 (EE -> IOP) for SPU2 transfers
 * and acknowledges each with command 1, type 0, echoing the request's opt word. */
uint8_t g_spu_ram[0x200000];

/* Per-frame sound updates: the EE DMAs a block to the IOP buffer named in the
 * last acknowledgement; the driver processes it on its next tick and answers
 * with the address of the other buffer. */
static const uint32_t snd_buf[2] = {0x000C8000u, 0x000CC000u};
static int snd_cur;
static volatile int snd_pending;
static void snd_process_update(const uint8_t *blk, uint32_t size);

static void snd_ack(uint32_t opt) {
    uint8_t r[24];
    memset(r, 0, sizeof r);
    wr(r, 0, sizeof r);
    wr(r, 2, 1);
    wr(r, 3, opt);
    wr(r, 4, 0);
    send_to_ee(r, sizeof r);
}

void iop_tick(void) {   /* called once per vblank */
    if (!snd_pending) return;
    snd_pending = 0;
    snd_process_update(g_iop_ram + snd_buf[snd_cur], 0x4000);
    snd_cur ^= 1;
    snd_ack(snd_buf[snd_cur]);
}

static void snd_process_update(const uint8_t *blk, uint32_t size) {
    static int n;
    if (getenv("HP_SNDDUMP") && n++ < 400 && (n % 50) == 1) {
        LOG("[sndupd] frame %d:\n", n);
        for (uint32_t k = 0; k < 2800; k += 32) {
            int nz = 0;
            for (uint32_t j = 0; j < 32; j++) nz |= blk[k + j];
            if (!nz) continue;
            LOG("  %04x:", k);
            for (uint32_t j = 0; j < 32; j += 4) LOG(" %08x", rd(blk + k, j / 4));
            LOG("\n");
        }
    }
}

static void spu_command(const uint8_t *p) {
    uint32_t psize = rd(p, 0) & 0xff, type = rd(p, 4);
    if (type == 1) {   /* {1, ?, iop src, size, spu dst} */
        uint32_t src = rd(p, 6) & 0x1fffff, size = rd(p, 7), dst = rd(p, 8);
        if (src + size <= IOP_RAM_SIZE && dst + size <= sizeof g_spu_ram) memcpy(g_spu_ram + dst, g_iop_ram + src, size);
        if (g_trace) LOG("[spu] transfer %u bytes IOP %06x -> SPU %06x\n", size, src, dst);
    } else {
        LOG("[spu] command type %u (psize %u):", type, psize);
        for (uint32_t k = 5; k < psize / 4 && k < 16; k++) LOG(" %08x", rd(p, k));
        LOG("\n");
    }
    snd_ack(snd_buf[snd_cur]);
}

static void handle_cmd(const uint8_t *p) {
    uint32_t cid = rd(p, 2);
    switch (cid) {
    case CMD_CHANGE_SADDR:
        ee_pktbuf = rd(p, 4);
        LOG("[sif] EE command buffer at %08x\n", ee_pktbuf);
        return;
    case CMD_SET_SREG:
        hw_sif_set_reg(0x80000000u + rd(p, 4), rd(p, 5));
        return;
    case CMD_INIT_CMD:
        LOG("[sif] init cmd: %08x %08x %08x %08x | %08x %08x %08x %08x\n", rd(p, 0), rd(p, 1), rd(p, 2), rd(p, 3),
            rd(p, 4), rd(p, 5), rd(p, 6), rd(p, 7));
        if (rd(p, 3) == 0) { ee_pktbuf = rd(p, 4); LOG("[sif] EE command buffer at %08x\n", ee_pktbuf); }
        else {
            /* sceSifInitRpc: the IOP answers with SET_SREG(0, 1) into the EE's soft registers */
            uint8_t r[24];
            memset(r, 0, sizeof r);
            wr(r, 0, sizeof r);
            wr(r, 2, CMD_SET_SREG);
            wr(r, 4, 0);
            wr(r, 5, 1);
            send_to_ee(r, sizeof r);
            hw_sif_set_reg(0x80000002u, 1);
        }
        return;
    case CMD_RESET_CMD:
        LOG("[sif] IOP reset requested: %.80s\n", (const char *)p + 24);
        hw_sif_iop_reset();
        return;
    case CMD_RPC_BIND: {
        uint32_t sid = rd(p, 8);
        Server *s = bind_server(sid);
        LOG("[rpc] bind %08x (%s)\n", sid, s ? s->name : "full");
        rpc_end(p, CMD_RPC_BIND, s ? 0x00008000u + 0x100u * (uint32_t)(s - servers) : 0, s ? s->buf : 0, s ? s->buf : 0);
        return;
    }
    case CMD_RPC_CALL: {
        uint32_t fno = rd(p, 8), send = rd(p, 9), recv_ee = rd(p, 10), recv_size = rd(p, 11);
        uint32_t server = rd(p, 13);
        int idx = (int)((server - 0x8000u) / 0x100u);
        Server *s = (idx >= 0 && idx < nservers) ? &servers[idx] : NULL;
        if (!s) { LOG("[rpc] call to unknown server %08x\n", server); rpc_end(p, CMD_RPC_CALL, 0, 0, 0); return; }
        static uint8_t out[0x10000];
        uint32_t out_len = 0;
        uint32_t cap = recv_size < sizeof out ? recv_size : sizeof out;
        if (g_trace) LOG("[rpc] call %s fn %u send %u recv %08x/%u\n", s->name, fno, send, recv_ee, recv_size);
        s->fn(fno, g_iop_ram + s->buf, send, out, cap, &out_len);
        if (recv_ee && out_len) memcpy(gptr(recv_ee), out, out_len < recv_size ? out_len : recv_size);
        rpc_end(p, CMD_RPC_CALL, 0, 0, 0);
        return;
    }
    case CMD_RPC_RDATA: {
        uint32_t src = rd(p, 8), dst = rd(p, 9), size = rd(p, 10);
        if (src + size <= IOP_RAM_SIZE) memcpy(gptr(dst), g_iop_ram + src, size);
        rpc_end(p, CMD_RPC_RDATA, 0, 0, 0);
        return;
    }
    }
    if (cid == 2) { snd_command(p); return; }
    if (cid == 0) { spu_command(p); return; }
    {
        uint32_t psize = rd(p, 0) & 0xff;
        LOG("[sif] user command %08x (psize %u):", cid, psize);
        for (uint32_t k = 4; k < psize / 4 && k < 16; k++) LOG(" %08x", rd(p, k));
        LOG("\n");
    }
}

/* sceSifSetDma(list, count): each entry {src, dest, size, attr} */
void iop_sif_dma(uint32_t list, int count) {
    for (int k = 0; k < count; k++) {
        uint32_t src = g32(list + 16 * k), dst = g32(list + 16 * k + 4), size = g32(list + 16 * k + 8);
        if (g_trace > 2) LOG("[sif] list %08x[%d]: %08x -> %08x size %u attr %08x\n", list, k, src, dst, size, g32(list + 16 * k + 12));
        dst &= 0x1fffffu;
        if (dst + size > IOP_RAM_SIZE) { LOG("[sif] dma to bad IOP address %08x+%x\n", dst, size); continue; }
        memcpy(g_iop_ram + dst, gptr(src), size);
        if (dst == IOP_CMDBUF) handle_cmd(g_iop_ram + dst);
        else if (dst == snd_buf[0] || dst == snd_buf[1]) snd_pending = 1;
        else if (g_trace) {
            LOG("[sif] dma %08x -> IOP %06x (%u bytes):", src, dst, size);
            for (uint32_t k = 0; k < size / 4 && k < 12; k++) LOG(" %08x", g32(src + 4 * k));
            LOG("\n");
        }
    }
}

void iop_servers_init(void);

void iop_init(void) {
    g_iop_ram = calloc(1, IOP_RAM_SIZE);
    hw_sif_set_smcom(IOP_CMDBUF);
    hw_sif_set_smflag(0x70000);   /* SIF init, command init, boot end */
    iop_servers_init();
}
