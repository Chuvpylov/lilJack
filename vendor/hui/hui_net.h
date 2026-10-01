/*
 * hui_net.h — Mesh networking layer for hui draw-lists
 *
 * Transmits hui_dl frames over UDP.  Supports peer discovery via UDP
 * broadcast and optional RLE compression of the command array.
 *
 * USAGE
 *   Include hui.h, then hui_proto.h, then this header.
 *   In exactly one translation unit define HUI_NET_IMPLEMENTATION:
 *
 *     #define HUI_IMPLEMENTATION
 *     #define HUI_BACKEND_HEADLESS
 *     #include "hui.h"
 *
 *     #define HUI_PROTO_IMPLEMENTATION
 *     #include "hui_proto.h"
 *
 *     #define HUI_NET_IMPLEMENTATION
 *     #include "hui_net.h"
 *
 * C99/C11.  POSIX sockets (Linux / macOS).  No third-party dependencies.
 */

#ifndef HUI_NET_H
#define HUI_NET_H

#ifndef HUI_PROTO_H
#  error "hui_net.h: include hui_proto.h (and hui.h) before hui_net.h"
#endif

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Constants
 * ========================================================================= */

#define HUI_NET_PORT_DEFAULT    7788u
#define HUI_NET_DISCOVERY_PORT  7789u
#define HUI_NET_MAX_DEVICES     16

/* Discovery packet: 4 magic + 32 name + 2 port + 1 role + 1 rsvd + 16 IP */
#define HUI_NET_DISC_SZ         56u
#define HUI_NET_DISC_MAGIC_0    0x48u   /* 'H' */
#define HUI_NET_DISC_MAGIC_1    0x4Eu   /* 'N' */
#define HUI_NET_DISC_MAGIC_2    0x45u   /* 'E' */
#define HUI_NET_DISC_MAGIC_3    0x54u   /* 'T' */

/* =========================================================================
 * Types
 * ========================================================================= */

typedef enum {
    HUI_NET_ROLE_SENDER   = 0,  /* sends draw lists */
    HUI_NET_ROLE_RECEIVER = 1,  /* receives draw lists */
    HUI_NET_ROLE_BOTH     = 2,  /* bidirectional */
} hui_net_role;

typedef struct {
    char     name[32];          /* human-readable node name */
    char     addr[16];          /* IP address string "x.x.x.x" */
    uint16_t port;              /* UDP port */
    hui_net_role role;
    uint64_t last_seen_ms;      /* monotonic milliseconds timestamp */
} hui_net_device;

typedef struct {
    int      sock_fd;           /* UDP data socket fd (-1 = invalid) */
    int      disc_fd;           /* discovery socket fd */
    uint16_t port;              /* bound data port */
    hui_net_role role;
    char     name[32];          /* this node's human-readable name */
    /* internal receive buffer — holds one pending frame */
    uint8_t  _rxbuf[HUI_PROTO_SCRATCH_SZ];
} hui_net_ctx;

/* =========================================================================
 * Core API declarations
 * ========================================================================= */

/*
 * Open UDP sockets.
 * port = 0 → HUI_NET_PORT_DEFAULT.
 * name: human-readable identifier for this node (shown in discovery).
 * Returns false on failure (check errno).
 */
bool hui_net_open(hui_net_ctx *ctx, uint16_t port,
                  hui_net_role role, const char *name);

/* Close both sockets and zero ctx. */
void hui_net_close(hui_net_ctx *ctx);

/*
 * Encode dl and send to addr:port over UDP.
 * Returns bytes sent on success, -1 on error.
 */
int hui_net_send(hui_net_ctx *ctx, const char *addr, uint16_t port,
                 const hui_dl *dl, uint32_t frame_seq);

/*
 * Non-blocking receive of one draw list.
 * Returns cmd_count (> 0) on success, 0 if no data ready, -1 on error.
 * dl_out is filled with the decoded frame.
 * src_addr_out (optional, must point to char[16]): sender IP string.
 */
int hui_net_recv(hui_net_ctx *ctx, hui_dl *dl_out, char *src_addr_out);

/*
 * Broadcast a discovery announcement on HUI_NET_DISCOVERY_PORT.
 * Call roughly once per second.
 */
void hui_net_announce(hui_net_ctx *ctx);

/*
 * Poll for discovery messages (non-blocking).
 * devices_out: caller-supplied array of HUI_NET_MAX_DEVICES entries.
 * *n_devices_out: updated with the number of known devices.
 * Returns number of new devices seen this call (0 if none).
 */
int hui_net_discover_poll(hui_net_ctx *ctx,
                          hui_net_device *devices_out, int *n_devices_out);

/* =========================================================================
 * RLE compression API
 * ========================================================================= */

/*
 * RLE-compress a hui_cmd array.
 * out must have capacity >= n * 33 bytes (worst case: every cmd unique +
 * one flag byte each).
 * Returns compressed size in bytes.
 *
 * Encoding unit:
 *   [uint8_t flags][32-byte cmd]
 *   flags bit 7 = 0 → single instance of cmd
 *   flags bit 7 = 1 → (flags & 0x7F) copies of cmd (value 1..127)
 */
uint32_t hui_net_rle_encode(const hui_cmd *cmds, uint16_t n,
                             uint8_t *out, uint32_t outsz);

/*
 * Decompress RLE-encoded commands.
 * cmds_out must hold at least max_cmds commands.
 * Returns decompressed cmd count, or 0 on error / overflow.
 */
uint16_t hui_net_rle_decode(const uint8_t *in, uint32_t insz,
                             hui_cmd *cmds_out, uint16_t max_cmds);

/* =========================================================================
 * SPI transport (Linux /dev/spidev) — Linux only
 * ========================================================================= */

#if defined(__linux__) && !defined(HUI_BAREMETAL)

typedef struct {
    int      fd;            /* spidev fd (-1 = closed) */
    uint32_t speed_hz;      /* SPI clock speed */
    uint8_t  _txbuf[HUI_PROTO_SCRATCH_SZ];
    uint8_t  _rxbuf[HUI_PROTO_SCRATCH_SZ];
} hui_spi_ctx;

/* Open SPI device (e.g. "/dev/spidev0.0").
 * speed_hz: clock speed (e.g. 4000000 for 4 MHz). Returns false on failure. */
bool hui_spi_open(hui_spi_ctx *ctx, const char *device, uint32_t speed_hz);

/* Close SPI device. */
void hui_spi_close(hui_spi_ctx *ctx);

/* Send a draw list over SPI.
 * Full-duplex: simultaneously receives into rxbuf; returns bytes sent or -1. */
int hui_spi_send(hui_spi_ctx *ctx, const hui_dl *dl, uint32_t frame_seq);

/* Receive a frame from SPI (polls; sends zero-filled TX to clock in data).
 * Returns cmd_count or 0 if nothing arrived, -1 on error. */
int hui_spi_recv(hui_spi_ctx *ctx, hui_dl *dl_out);

/* =========================================================================
 * USB CDC serial transport (POSIX termios) — Linux only
 * ========================================================================= */

typedef struct {
    int      fd;
    uint8_t  _txbuf[HUI_PROTO_SCRATCH_SZ];
    uint8_t  _rxbuf[HUI_PROTO_SCRATCH_SZ];
    int      _rx_fill;    /* bytes buffered in _rxbuf waiting for frame */
} hui_usb_ctx;

/* Open serial device (e.g. "/dev/ttyACM0").
 * baud: e.g. 115200 or 921600. Uses 8N1, raw mode. Returns false on failure. */
bool hui_usb_open(hui_usb_ctx *ctx, const char *device, int baud);

/* Close. */
void hui_usb_close(hui_usb_ctx *ctx);

/* Send draw list as length-prefixed COBS frame.
 * Returns bytes written or -1. */
int hui_usb_send(hui_usb_ctx *ctx, const hui_dl *dl, uint32_t frame_seq);

/* Receive one frame (non-blocking; polls with O_NONBLOCK).
 * Accumulates partial data in ctx->_rx_fill.
 * Returns cmd_count or 0 if frame not yet complete, -1 on error. */
int hui_usb_recv(hui_usb_ctx *ctx, hui_dl *dl_out);

#endif /* defined(__linux__) && !defined(HUI_BAREMETAL) */

/* =========================================================================
 * Implementation
 * ========================================================================= */

#ifdef HUI_NET_IMPLEMENTATION

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

static uint64_t hui__net_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static bool hui__net_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

/* Create, configure and bind a UDP socket. Returns fd or -1 on error. */
static int hui__net_make_udp(uint16_t port, bool broadcast)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    int optval = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    if (broadcast) {
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &optval, sizeof(optval));
    }

    if (!hui__net_set_nonblocking(fd)) {
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

/* -------------------------------------------------------------------------
 * hui_net_open
 * ------------------------------------------------------------------------- */
bool hui_net_open(hui_net_ctx *ctx, uint16_t port,
                  hui_net_role role, const char *name)
{
    if (!ctx) return false;
    memset(ctx, 0, sizeof(*ctx));
    ctx->sock_fd = -1;
    ctx->disc_fd = -1;

    if (port == 0) port = (uint16_t)HUI_NET_PORT_DEFAULT;
    ctx->port = port;
    ctx->role = role;

    if (name) {
        strncpy(ctx->name, name, sizeof(ctx->name) - 1);
        ctx->name[sizeof(ctx->name) - 1] = '\0';
    }

    /* Data socket */
    ctx->sock_fd = hui__net_make_udp(port, false);
    if (ctx->sock_fd < 0) return false;

    /* Discovery socket — also needs SO_BROADCAST for sending */
    ctx->disc_fd = hui__net_make_udp((uint16_t)HUI_NET_DISCOVERY_PORT, true);
    if (ctx->disc_fd < 0) {
        close(ctx->sock_fd);
        ctx->sock_fd = -1;
        return false;
    }

    return true;
}

/* -------------------------------------------------------------------------
 * hui_net_close
 * ------------------------------------------------------------------------- */
void hui_net_close(hui_net_ctx *ctx)
{
    if (!ctx) return;
    if (ctx->sock_fd >= 0) { close(ctx->sock_fd); ctx->sock_fd = -1; }
    if (ctx->disc_fd >= 0) { close(ctx->disc_fd); ctx->disc_fd = -1; }
}

/* -------------------------------------------------------------------------
 * hui_net_send
 * ------------------------------------------------------------------------- */
int hui_net_send(hui_net_ctx *ctx, const char *addr, uint16_t port,
                 const hui_dl *dl, uint32_t frame_seq)
{
    if (!ctx || ctx->sock_fd < 0 || !addr || !dl) return -1;

    /* Encode into a static scratch buffer.
     * We use a local static here to avoid a large stack allocation.
     * This is not thread-safe — fine for single-threaded hui usage. */
    static uint8_t hui__net_txbuf[HUI_PROTO_SCRATCH_SZ];

    uint32_t encoded = hui_proto_encode(dl, hui__net_txbuf,
                                        HUI_PROTO_SCRATCH_SZ, frame_seq);
    if (encoded == 0) {
        fprintf(stderr, "hui_net_send: encode failed (frame too large?)\n");
        return -1;
    }

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(port);
    if (inet_pton(AF_INET, addr, &dest.sin_addr) != 1) return -1;

    ssize_t sent = sendto(ctx->sock_fd, hui__net_txbuf, encoded, 0,
                          (struct sockaddr *)&dest, sizeof(dest));
    return (int)sent;
}

/* -------------------------------------------------------------------------
 * hui_net_recv
 * ------------------------------------------------------------------------- */
int hui_net_recv(hui_net_ctx *ctx, hui_dl *dl_out, char *src_addr_out)
{
    if (!ctx || ctx->sock_fd < 0 || !dl_out) return -1;

    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);

    ssize_t nbytes = recvfrom(ctx->sock_fd, ctx->_rxbuf,
                              sizeof(ctx->_rxbuf), 0,
                              (struct sockaddr *)&from, &fromlen);

    if (nbytes < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }

    if (src_addr_out) {
        inet_ntop(AF_INET, &from.sin_addr, src_addr_out, 16);
    }

    const hui_cmd  *cmds     = NULL;
    const char     *strpool  = NULL;
    const float    *datapool = NULL;
    hui_proto_header hdr;
    memset(&hdr, 0, sizeof(hdr));

    uint16_t count = hui_proto_decode(ctx->_rxbuf, (uint32_t)nbytes,
                                      &cmds, &strpool, &datapool, &hdr);

    /* hui_proto_decode returns 0 for both error and truly-empty frames.
     * Distinguish via hdr.cmd_count after the call. */
    if (count == 0 && hdr.cmd_count == 0) return -1;

    uint32_t cmd_bytes      = (uint32_t)hdr.cmd_count * 32u;
    uint32_t strpool_bytes  = hdr.strpool_len;
    uint32_t datapool_bytes = hdr.datapool_len * 4u;

    /* Guard against overflow into dl_out — caller must have enough capacity */
#ifndef HUI_BAREMETAL
    if (hdr.cmd_count > dl_out->cap ||
        hdr.strpool_len > dl_out->strpool_cap ||
        hdr.datapool_len > dl_out->datapool_cap) {
        fprintf(stderr, "hui_net_recv: dl_out capacity too small\n");
        return -1;
    }
#endif

    memcpy(dl_out->cmds,     cmds,     cmd_bytes);
    memcpy(dl_out->strpool,  strpool,  strpool_bytes);
    memcpy(dl_out->datapool, datapool, datapool_bytes);

    dl_out->count        = hdr.cmd_count;
    dl_out->strpool_len  = (uint16_t)hdr.strpool_len;
    dl_out->datapool_len = hdr.datapool_len;

    return (int)hdr.cmd_count;
}

/* -------------------------------------------------------------------------
 * Discovery helpers
 * ------------------------------------------------------------------------- */

/* Pack a discovery message into buf[HUI_NET_DISC_SZ]. */
static void hui__net_pack_disc(const hui_net_ctx *ctx,
                                const char *ip_str, uint8_t *buf)
{
    memset(buf, 0, HUI_NET_DISC_SZ);
    buf[0] = HUI_NET_DISC_MAGIC_0;
    buf[1] = HUI_NET_DISC_MAGIC_1;
    buf[2] = HUI_NET_DISC_MAGIC_2;
    buf[3] = HUI_NET_DISC_MAGIC_3;

    memcpy(buf + 4, ctx->name, 32);                  /* name (zero-padded) */

    buf[36] = (uint8_t)(ctx->port & 0xFFu);          /* port LE */
    buf[37] = (uint8_t)((ctx->port >> 8) & 0xFFu);

    buf[38] = (uint8_t)ctx->role;                    /* role */
    buf[39] = 0;                                     /* reserved */

    if (ip_str) {
        strncpy((char *)(buf + 40), ip_str, 15);     /* IP string (16 bytes) */
        buf[55] = '\0';
    }
}

static bool hui__net_check_disc_magic(const uint8_t *buf)
{
    return buf[0] == HUI_NET_DISC_MAGIC_0 &&
           buf[1] == HUI_NET_DISC_MAGIC_1 &&
           buf[2] == HUI_NET_DISC_MAGIC_2 &&
           buf[3] == HUI_NET_DISC_MAGIC_3;
}

/* -------------------------------------------------------------------------
 * hui_net_announce
 * ------------------------------------------------------------------------- */
void hui_net_announce(hui_net_ctx *ctx)
{
    if (!ctx || ctx->disc_fd < 0) return;

    /* Determine our own IP by reading the source of an outbound connect.
     * This is a standard trick that works without needing getifaddrs. */
    char ip_str[16] = "0.0.0.0";
    {
        int tmp = socket(AF_INET, SOCK_DGRAM, 0);
        if (tmp >= 0) {
            struct sockaddr_in probe;
            memset(&probe, 0, sizeof(probe));
            probe.sin_family      = AF_INET;
            probe.sin_port        = htons(1);
            inet_pton(AF_INET, "8.8.8.8", &probe.sin_addr);
            if (connect(tmp, (struct sockaddr *)&probe, sizeof(probe)) == 0) {
                struct sockaddr_in local;
                socklen_t local_len = sizeof(local);
                if (getsockname(tmp, (struct sockaddr *)&local, &local_len) == 0) {
                    inet_ntop(AF_INET, &local.sin_addr, ip_str, sizeof(ip_str));
                }
            }
            close(tmp);
        }
    }

    uint8_t buf[HUI_NET_DISC_SZ];
    hui__net_pack_disc(ctx, ip_str, buf);

    struct sockaddr_in bcast;
    memset(&bcast, 0, sizeof(bcast));
    bcast.sin_family      = AF_INET;
    bcast.sin_port        = htons(HUI_NET_DISCOVERY_PORT);
    bcast.sin_addr.s_addr = INADDR_BROADCAST;

    sendto(ctx->disc_fd, buf, HUI_NET_DISC_SZ, 0,
           (struct sockaddr *)&bcast, sizeof(bcast));
}

/* -------------------------------------------------------------------------
 * hui_net_discover_poll
 * ------------------------------------------------------------------------- */
int hui_net_discover_poll(hui_net_ctx *ctx,
                          hui_net_device *devices_out, int *n_devices_out)
{
    if (!ctx || ctx->disc_fd < 0 || !devices_out || !n_devices_out) return 0;

    int new_count = 0;

    for (;;) {
        uint8_t buf[HUI_NET_DISC_SZ + 8]; /* a little slack */
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);

        ssize_t nbytes = recvfrom(ctx->disc_fd, buf, sizeof(buf), 0,
                                  (struct sockaddr *)&from, &fromlen);
        if (nbytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            break;  /* real error — stop polling */
        }

        if ((uint32_t)nbytes < HUI_NET_DISC_SZ) continue;
        if (!hui__net_check_disc_magic(buf)) continue;

        /* Parse fields */
        char     name[32];
        uint16_t port;
        uint8_t  role;
        char     ip_str[16];

        memcpy(name, buf + 4, 32);
        name[31] = '\0';

        port = (uint16_t)(buf[36] | ((uint16_t)buf[37] << 8));
        role = buf[38];

        /* Prefer the IP from the packet payload; fall back to source addr */
        memcpy(ip_str, buf + 40, 16);
        ip_str[15] = '\0';
        if (ip_str[0] == '\0' || ip_str[0] == '0') {
            inet_ntop(AF_INET, &from.sin_addr, ip_str, sizeof(ip_str));
        }

        uint64_t now_ms = hui__net_now_ms();

        /* Look for existing entry (match on addr+port) */
        int found_idx = -1;
        for (int i = 0; i < *n_devices_out; i++) {
            if (strncmp(devices_out[i].addr, ip_str, 16) == 0 &&
                devices_out[i].port == port) {
                found_idx = i;
                break;
            }
        }

        if (found_idx >= 0) {
            /* Update existing */
            devices_out[found_idx].last_seen_ms = now_ms;
            memcpy(devices_out[found_idx].name, name, 32);
            devices_out[found_idx].role = (hui_net_role)role;
        } else {
            /* New device */
            if (*n_devices_out < HUI_NET_MAX_DEVICES) {
                hui_net_device *d = &devices_out[*n_devices_out];
                memset(d, 0, sizeof(*d));
                memcpy(d->name, name, 32);
                memcpy(d->addr, ip_str, 16);
                d->port         = port;
                d->role         = (hui_net_role)role;
                d->last_seen_ms = now_ms;
                (*n_devices_out)++;
                new_count++;
            }
        }
    }

    return new_count;
}

/* -------------------------------------------------------------------------
 * RLE compression
 * ------------------------------------------------------------------------- */

uint32_t hui_net_rle_encode(const hui_cmd *cmds, uint16_t n,
                             uint8_t *out, uint32_t outsz)
{
    if (!cmds || !out || n == 0) return 0;

    uint32_t wp = 0; /* write position */
    uint16_t i  = 0;

    while (i < n) {
        /* Count run length (max 127 due to 7-bit field) */
        uint16_t run = 1;
        while (run < 127 && (i + run) < n &&
               memcmp(&cmds[i], &cmds[i + run], 32) == 0) {
            run++;
        }

        /* Need 1 flag byte + 32 data bytes */
        if (wp + 33u > outsz) return 0; /* output buffer too small */

        if (run > 1) {
            out[wp++] = (uint8_t)(0x80u | (run & 0x7Fu)); /* flags: run */
        } else {
            out[wp++] = 0x00u;                             /* flags: single */
        }
        memcpy(out + wp, &cmds[i], 32);
        wp += 32;

        i = (uint16_t)(i + run);
    }

    return wp;
}

uint16_t hui_net_rle_decode(const uint8_t *in, uint32_t insz,
                             hui_cmd *cmds_out, uint16_t max_cmds)
{
    if (!in || !cmds_out || insz == 0) return 0;

    uint32_t rp    = 0; /* read position */
    uint16_t count = 0;

    while (rp < insz) {
        if (rp + 33u > insz) return 0; /* truncated packet */

        uint8_t flags = in[rp++];
        uint16_t run;

        if (flags & 0x80u) {
            run = (uint16_t)(flags & 0x7Fu);
            if (run == 0) run = 1; /* guard against malformed run=0 */
        } else {
            run = 1;
        }

        if (count + run > max_cmds) return 0; /* would overflow output */

        const hui_cmd *src = (const hui_cmd *)(const void *)(in + rp);
        for (uint16_t k = 0; k < run; k++) {
            memcpy(&cmds_out[count++], src, 32);
        }
        rp += 32;
    }

    return count;
}

#if defined(__linux__) && !defined(HUI_BAREMETAL)

/* -------------------------------------------------------------------------
 * SPI transport implementation
 * ------------------------------------------------------------------------- */

#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

bool hui_spi_open(hui_spi_ctx *ctx, const char *device, uint32_t speed_hz)
{
    if (!ctx || !device) return false;
    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = -1;

    int fd = open(device, O_RDWR);
    if (fd < 0) return false;

    uint8_t mode = SPI_MODE_0;
    if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0) { close(fd); return false; }

    uint32_t speed = speed_hz;
    if (ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) { close(fd); return false; }

    uint8_t bits = 8;
    if (ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0) { close(fd); return false; }

    ctx->fd       = fd;
    ctx->speed_hz = speed_hz;
    return true;
}

void hui_spi_close(hui_spi_ctx *ctx)
{
    if (!ctx) return;
    if (ctx->fd >= 0) { close(ctx->fd); ctx->fd = -1; }
}

int hui_spi_send(hui_spi_ctx *ctx, const hui_dl *dl, uint32_t frame_seq)
{
    if (!ctx || ctx->fd < 0 || !dl) return -1;

    uint32_t encoded = hui_proto_encode(dl, ctx->_txbuf,
                                        HUI_PROTO_SCRATCH_SZ, frame_seq);
    if (encoded == 0) {
        fprintf(stderr, "hui_spi_send: encode failed (frame too large?)\n");
        return -1;
    }

    struct spi_ioc_transfer xfer;
    memset(&xfer, 0, sizeof(xfer));
    xfer.tx_buf        = (unsigned long)ctx->_txbuf;
    xfer.rx_buf        = (unsigned long)ctx->_rxbuf;
    xfer.len           = encoded;
    xfer.speed_hz      = ctx->speed_hz;
    xfer.bits_per_word = 8;

    if (ioctl(ctx->fd, SPI_IOC_MESSAGE(1), &xfer) < 0) return -1;
    return (int)encoded;
}

int hui_spi_recv(hui_spi_ctx *ctx, hui_dl *dl_out)
{
    if (!ctx || ctx->fd < 0 || !dl_out) return -1;

    /* Poll by clocking out zeros; remote can fill rxbuf with a frame. */
    memset(ctx->_txbuf, 0, HUI_PROTO_SCRATCH_SZ);

    struct spi_ioc_transfer xfer;
    memset(&xfer, 0, sizeof(xfer));
    xfer.tx_buf        = (unsigned long)ctx->_txbuf;
    xfer.rx_buf        = (unsigned long)ctx->_rxbuf;
    xfer.len           = HUI_PROTO_SCRATCH_SZ;
    xfer.speed_hz      = ctx->speed_hz;
    xfer.bits_per_word = 8;

    if (ioctl(ctx->fd, SPI_IOC_MESSAGE(1), &xfer) < 0) return -1;

    const hui_cmd  *cmds     = NULL;
    const char     *strpool  = NULL;
    const float    *datapool = NULL;
    hui_proto_header hdr;
    memset(&hdr, 0, sizeof(hdr));

    uint16_t count = hui_proto_decode(ctx->_rxbuf, HUI_PROTO_SCRATCH_SZ,
                                      &cmds, &strpool, &datapool, &hdr);

    /* If magic doesn't match, nothing useful arrived. */
    if (count == 0 && hdr.cmd_count == 0) return 0;

#ifndef HUI_BAREMETAL
    if (hdr.cmd_count > dl_out->cap ||
        hdr.strpool_len > dl_out->strpool_cap ||
        hdr.datapool_len > dl_out->datapool_cap) {
        fprintf(stderr, "hui_spi_recv: dl_out capacity too small\n");
        return -1;
    }
#endif

    memcpy(dl_out->cmds,     cmds,     (uint32_t)hdr.cmd_count * 32u);
    memcpy(dl_out->strpool,  strpool,  hdr.strpool_len);
    memcpy(dl_out->datapool, datapool, hdr.datapool_len * 4u);

    dl_out->count        = hdr.cmd_count;
    dl_out->strpool_len  = (uint16_t)hdr.strpool_len;
    dl_out->datapool_len = hdr.datapool_len;

    return (int)hdr.cmd_count;
}

/* -------------------------------------------------------------------------
 * USB CDC serial transport implementation
 * ------------------------------------------------------------------------- */

#include <termios.h>
#include <sys/select.h>

static speed_t hui__baud_to_speed(int baud)
{
    switch (baud) {
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        case 460800: return B460800;
        case 921600: return B921600;
        default:     return B115200;
    }
}

bool hui_usb_open(hui_usb_ctx *ctx, const char *device, int baud)
{
    if (!ctx || !device) return false;
    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = -1;

    int fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return false;

    struct termios tty;
    memset(&tty, 0, sizeof(tty));

    speed_t speed = hui__baud_to_speed(baud);
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);
    cfmakeraw(&tty);
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tty) < 0) { close(fd); return false; }

    ctx->fd = fd;
    return true;
}

void hui_usb_close(hui_usb_ctx *ctx)
{
    if (!ctx) return;
    if (ctx->fd >= 0) { close(ctx->fd); ctx->fd = -1; }
    ctx->_rx_fill = 0;
}

int hui_usb_send(hui_usb_ctx *ctx, const hui_dl *dl, uint32_t frame_seq)
{
    if (!ctx || ctx->fd < 0 || !dl) return -1;

    /* Encode raw packet into txbuf. */
    uint32_t encoded = hui_proto_encode(dl, ctx->_txbuf,
                                        HUI_PROTO_SCRATCH_SZ, frame_seq);
    if (encoded == 0) {
        fprintf(stderr, "hui_usb_send: encode failed (frame too large?)\n");
        return -1;
    }

    /* COBS-encode so the stream contains no 0x00 bytes mid-frame. */
    static uint8_t hui__usb_cobsbuf[HUI_PROTO_SCRATCH_SZ + 2];
    uint32_t cobs_len = hui_proto_cobs_encode(ctx->_txbuf, encoded,
                                              hui__usb_cobsbuf,
                                              sizeof(hui__usb_cobsbuf));
    if (cobs_len == 0) {
        fprintf(stderr, "hui_usb_send: COBS encode failed\n");
        return -1;
    }

    /* Append 0x00 frame delimiter. */
    hui__usb_cobsbuf[cobs_len] = 0x00;

    ssize_t written = write(ctx->fd, hui__usb_cobsbuf, cobs_len + 1u);
    return (int)written;
}

int hui_usb_recv(hui_usb_ctx *ctx, hui_dl *dl_out)
{
    if (!ctx || ctx->fd < 0 || !dl_out) return -1;

    /* Read available bytes (non-blocking). */
    int space = (int)sizeof(ctx->_rxbuf) - ctx->_rx_fill;
    if (space > 0) {
        ssize_t n = read(ctx->fd,
                         ctx->_rxbuf + ctx->_rx_fill,
                         (size_t)space);
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return -1;
        if (n > 0) ctx->_rx_fill += (int)n;
    }

    /* Scan for 0x00 frame delimiter. */
    int delim = -1;
    for (int i = 0; i < ctx->_rx_fill; i++) {
        if (ctx->_rxbuf[i] == 0x00) { delim = i; break; }
    }
    if (delim < 0) return 0; /* frame not yet complete */

    /* COBS-decode the frame (bytes before the delimiter). */
    static uint8_t hui__usb_decbuf[HUI_PROTO_SCRATCH_SZ];
    uint32_t dec_len = hui_proto_cobs_decode(ctx->_rxbuf, (uint32_t)delim,
                                             hui__usb_decbuf,
                                             sizeof(hui__usb_decbuf));

    /* Shift remaining bytes (after delimiter) to front of rxbuf. */
    int remaining = ctx->_rx_fill - delim - 1;
    if (remaining > 0) {
        memmove(ctx->_rxbuf, ctx->_rxbuf + delim + 1, (size_t)remaining);
    }
    ctx->_rx_fill = remaining > 0 ? remaining : 0;

    if (dec_len == 0) return 0; /* COBS error or empty frame */

    const hui_cmd  *cmds     = NULL;
    const char     *strpool  = NULL;
    const float    *datapool = NULL;
    hui_proto_header hdr;
    memset(&hdr, 0, sizeof(hdr));

    uint16_t count = hui_proto_decode(hui__usb_decbuf, dec_len,
                                      &cmds, &strpool, &datapool, &hdr);

    if (count == 0 && hdr.cmd_count == 0) return -1;

#ifndef HUI_BAREMETAL
    if (hdr.cmd_count > dl_out->cap ||
        hdr.strpool_len > dl_out->strpool_cap ||
        hdr.datapool_len > dl_out->datapool_cap) {
        fprintf(stderr, "hui_usb_recv: dl_out capacity too small\n");
        return -1;
    }
#endif

    memcpy(dl_out->cmds,     cmds,     (uint32_t)hdr.cmd_count * 32u);
    memcpy(dl_out->strpool,  strpool,  hdr.strpool_len);
    memcpy(dl_out->datapool, datapool, hdr.datapool_len * 4u);

    dl_out->count        = hdr.cmd_count;
    dl_out->strpool_len  = (uint16_t)hdr.strpool_len;
    dl_out->datapool_len = hdr.datapool_len;

    return (int)hdr.cmd_count;
}

#endif /* defined(__linux__) && !defined(HUI_BAREMETAL) */

#endif /* HUI_NET_IMPLEMENTATION */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* HUI_NET_H */
