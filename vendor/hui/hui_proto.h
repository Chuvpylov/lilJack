/*
 * hui_proto.h — Protocol framing layer for hui draw-lists
 *
 * Serializes hui_dl to a flat byte stream suitable for UART, UDP, SPI, or
 * file replay.  Two framing helpers are provided:
 *   - Length-prefix framing  (TCP / UDP / file)
 *   - COBS framing           (UART / serial — zero-free bytestream)
 *
 * USAGE
 *   Include hui.h (or at minimum hui_draw.h) BEFORE this header.
 *   In exactly one translation unit define HUI_PROTO_IMPLEMENTATION:
 *
 *     #define HUI_IMPLEMENTATION
 *     #define HUI_BACKEND_HEADLESS
 *     #include "hui.h"
 *
 *     #define HUI_PROTO_IMPLEMENTATION
 *     #include "hui_proto.h"
 *
 * WIRE FORMAT
 *   [4 B]  magic  : 0x48 0x55 0x49 0x31  ("HUI1")
 *   [1 B]  version: 1
 *   [4 B]  frame_seq  (little-endian uint32)
 *   [2 B]  cmd_count  (little-endian uint16)
 *   [4 B]  strpool_len  (little-endian uint32)
 *   [4 B]  datapool_len (little-endian uint32, number of floats)
 *   ---- header total: HUI_PROTO_HEADER_SZ (15 bytes) ----
 *   [cmd_count * 32 B]    raw hui_cmd array
 *   [strpool_len B]       raw strpool bytes
 *   [datapool_len * 4 B]  raw datapool floats
 *
 * C99/C11.  No heap, no external dependencies.
 */

#ifndef HUI_PROTO_H
#define HUI_PROTO_H

/* Require hui_draw.h types */
#ifndef HUI_DRAW_H
#  error "hui_proto.h: include hui.h (or hui_draw.h) before hui_proto.h"
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Constants
 * ========================================================================= */

#define HUI_PROTO_MAGIC_0   0x48u
#define HUI_PROTO_MAGIC_1   0x55u
#define HUI_PROTO_MAGIC_2   0x49u
#define HUI_PROTO_MAGIC_3   0x31u
#define HUI_PROTO_VERSION   1
#define HUI_PROTO_HEADER_SZ 15

/* Scratch buffer used by hui_proto_flush_to_file / hui_proto_replay_from_file.
 * Override before including this header if needed.
 * 256 KiB handles ~8000 small commands + modest string/data pools. */
#ifndef HUI_PROTO_SCRATCH_SZ
#  define HUI_PROTO_SCRATCH_SZ (256u * 1024u)
#endif

/* Theoretical max for a fully-loaded draw list (informational only).
 * HUI_MAX_CMDS=4096, HUI_STRPOOL=8192, HUI_DATAPOOL=32768 (desktop defaults). */
#define HUI_PROTO_MAX_WIRE_SZ \
    (HUI_PROTO_HEADER_SZ + HUI_MAX_CMDS * 32u + HUI_STRPOOL + HUI_DATAPOOL * 4u)

/* =========================================================================
 * Types
 * ========================================================================= */

typedef struct {
    uint32_t frame_seq;
    uint16_t cmd_count;
    uint32_t strpool_len;
    uint32_t datapool_len;  /* number of floats */
} hui_proto_header;

/* =========================================================================
 * Little-endian helpers (inline, no external deps)
 * ========================================================================= */

static inline void hui__proto_put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline void hui__proto_put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >>  8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static inline uint16_t hui__proto_get_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t hui__proto_get_u32(const uint8_t *p) {
    return (uint32_t)( (uint32_t)p[0]
                     | ((uint32_t)p[1] <<  8)
                     | ((uint32_t)p[2] << 16)
                     | ((uint32_t)p[3] << 24) );
}

/* =========================================================================
 * Core encode / decode declarations
 * ========================================================================= */

/*
 * Serialize dl into buf.
 *
 * Returns bytes written on success, 0 if buf is too small.
 * Recommended buf size: HUI_PROTO_HEADER_SZ + dl->count*32 + dl->strpool_len
 *                       + dl->datapool_len*4
 */
uint32_t hui_proto_encode(const hui_dl *dl, uint8_t *buf, uint32_t bufsz,
                          uint32_t frame_seq);

/*
 * Parse a packet in buf[0..len-1].
 *
 * On success:
 *   *cmds_out      — pointer into buf at the start of the cmd array
 *   *strpool_out   — pointer into buf at the start of the strpool
 *   *datapool_out  — pointer into buf at the start of the datapool floats
 *   *hdr_out       — filled with parsed header fields
 *
 * Returns cmd_count (>= 0) on success, 0 on any error (including 0 cmds).
 * Caller can distinguish "0 cmds" from error via hdr_out->cmd_count.
 * All pointer outputs point INTO buf — no copies made (zero-copy).
 */
uint16_t hui_proto_decode(const uint8_t *buf, uint32_t len,
                          const hui_cmd    **cmds_out,
                          const char       **strpool_out,
                          const float      **datapool_out,
                          hui_proto_header  *hdr_out);

/* =========================================================================
 * COBS framing declarations
 * ========================================================================= */

/*
 * COBS-encode src[0..len-1] into dst.
 *
 * dst must have space for at least len + 1 + (len / 254) bytes.
 * The output contains no 0x00 bytes; the caller appends 0x00 as delimiter.
 * Returns the number of bytes written to dst.
 */
uint32_t hui_proto_cobs_encode(const uint8_t *src, uint32_t len, uint8_t *dst);

/*
 * COBS-decode src[0..len-1] into dst.
 *
 * src must NOT contain a trailing 0x00 delimiter (strip it before calling).
 * Returns decoded length, or 0 on encoding error.
 */
uint32_t hui_proto_cobs_decode(const uint8_t *src, uint32_t len, uint8_t *dst);

/* =========================================================================
 * Length-prefix framing declarations (TCP / file)
 * ========================================================================= */

/*
 * Write a 4-byte little-endian length prefix followed by payload to fp.
 * Returns total bytes written (4 + payload_len), or 0 on error.
 */
uint32_t hui_proto_write_frame(FILE *fp, const uint8_t *payload,
                               uint32_t payload_len);

/*
 * Read one length-prefixed frame from fp into buf (max bufsz bytes).
 * Returns payload bytes read, or 0 on EOF / error / buffer too small.
 */
uint32_t hui_proto_read_frame(FILE *fp, uint8_t *buf, uint32_t bufsz);

/* =========================================================================
 * Convenience wrappers (use internal scratch buffer)
 * ========================================================================= */

/*
 * Encode the global hui draw list (hui_dl_g) and write it as a
 * length-prefixed frame to fp.
 *
 * Uses an internal HUI_PROTO_SCRATCH_SZ static buffer.
 * If the encoded frame exceeds the scratch size the call returns false and
 * nothing is written to fp.
 */
bool hui_proto_flush_to_file(FILE *fp, uint32_t frame_seq);

/*
 * Read one length-prefixed frame from fp, decode it, and copy the draw
 * commands / strpool / datapool into dl_out.
 *
 * Returns the number of commands replayed, or 0 on error.
 * dl_out must already be initialised (hui_dl_init) with sufficient capacity.
 */
uint16_t hui_proto_replay_from_file(FILE *fp, hui_dl *dl_out);

/* =========================================================================
 * Implementation
 * ========================================================================= */

#ifdef HUI_PROTO_IMPLEMENTATION

/* Internal scratch buffer shared by flush/replay helpers */
static uint8_t hui__proto_scratch[HUI_PROTO_SCRATCH_SZ];

/* -------------------------------------------------------------------------
 * hui_proto_encode
 * ------------------------------------------------------------------------- */
uint32_t hui_proto_encode(const hui_dl *dl, uint8_t *buf, uint32_t bufsz,
                          uint32_t frame_seq)
{
    uint32_t cmd_bytes      = (uint32_t)dl->count * 32u;
    uint32_t strpool_bytes  = (uint32_t)dl->strpool_len;
    uint32_t datapool_bytes = (uint32_t)dl->datapool_len * 4u;
    uint32_t total          = HUI_PROTO_HEADER_SZ + cmd_bytes
                            + strpool_bytes + datapool_bytes;

    if (bufsz < total) return 0;

    uint8_t *p = buf;

    /* Magic + version */
    *p++ = HUI_PROTO_MAGIC_0;
    *p++ = HUI_PROTO_MAGIC_1;
    *p++ = HUI_PROTO_MAGIC_2;
    *p++ = HUI_PROTO_MAGIC_3;
    *p++ = HUI_PROTO_VERSION;

    /* Header fields */
    hui__proto_put_u32(p, frame_seq);    p += 4;
    hui__proto_put_u16(p, dl->count);   p += 2;
    hui__proto_put_u32(p, dl->strpool_len);  p += 4;
    hui__proto_put_u32(p, dl->datapool_len); p += 4;

    /* Payload */
    memcpy(p, dl->cmds,     cmd_bytes);      p += cmd_bytes;
    memcpy(p, dl->strpool,  strpool_bytes);  p += strpool_bytes;
    memcpy(p, dl->datapool, datapool_bytes);

    return total;
}

/* -------------------------------------------------------------------------
 * hui_proto_decode
 * ------------------------------------------------------------------------- */
uint16_t hui_proto_decode(const uint8_t *buf, uint32_t len,
                          const hui_cmd    **cmds_out,
                          const char       **strpool_out,
                          const float      **datapool_out,
                          hui_proto_header  *hdr_out)
{
    /* Minimum: header must fit */
    if (len < HUI_PROTO_HEADER_SZ) return 0;

    const uint8_t *p = buf;

    /* Validate magic */
    if (p[0] != HUI_PROTO_MAGIC_0 || p[1] != HUI_PROTO_MAGIC_1 ||
        p[2] != HUI_PROTO_MAGIC_2 || p[3] != HUI_PROTO_MAGIC_3) return 0;
    if (p[4] != HUI_PROTO_VERSION) return 0;
    p += 5;

    hui_proto_header hdr;
    hdr.frame_seq    = hui__proto_get_u32(p); p += 4;
    hdr.cmd_count    = hui__proto_get_u16(p); p += 2;
    hdr.strpool_len  = hui__proto_get_u32(p); p += 4;
    hdr.datapool_len = hui__proto_get_u32(p); /* p += 4 — not used after this */

    /* Validate payload fits in buf */
    uint32_t cmd_bytes      = (uint32_t)hdr.cmd_count    * 32u;
    uint32_t strpool_bytes  = hdr.strpool_len;
    uint32_t datapool_bytes = hdr.datapool_len * 4u;
    uint32_t payload_total  = cmd_bytes + strpool_bytes + datapool_bytes;

    if ((uint32_t)(HUI_PROTO_HEADER_SZ) + payload_total > len) return 0;

    /* Zero-copy: point into buf */
    const uint8_t *payload = buf + HUI_PROTO_HEADER_SZ;
    *cmds_out     = (const hui_cmd *)(const void *)payload;
    *strpool_out  = (const char *)(payload + cmd_bytes);
    *datapool_out = (const float *)(const void *)(payload + cmd_bytes + strpool_bytes);

    if (hdr_out) *hdr_out = hdr;

    return hdr.cmd_count;
}

/* -------------------------------------------------------------------------
 * COBS encode
 *
 * Standard COBS (Consistent Overhead Byte Stuffing):
 *   Divide src into chunks delimited by 0x00 bytes (or end of input).
 *   Each chunk is preceded by a code byte = distance to the next 0x00
 *   (or end-of-packet marker).  If chunk length reaches 254 an implicit
 *   continuation code 0xFF is inserted.
 *
 * Reference: Cheshire & Baker, "Consistent Overhead Byte Stuffing", 1999.
 * ------------------------------------------------------------------------- */
uint32_t hui_proto_cobs_encode(const uint8_t *src, uint32_t len, uint8_t *dst)
{
    const uint8_t *src_end = src + len;
    uint8_t       *out     = dst;

    /* Pointer to the current overhead/code byte we are filling in */
    uint8_t *code_ptr = out++;
    uint8_t  code     = 1;

    while (src < src_end) {
        if (*src == 0x00) {
            /* Terminate current run */
            *code_ptr = code;
            code_ptr  = out++;
            code      = 1;
            src++;
        } else {
            *out++ = *src++;
            code++;
            if (code == 0xFF) {
                /* Maximum run length reached — emit implicit continuation */
                *code_ptr = code;
                code_ptr  = out++;
                code      = 1;
            }
        }
    }

    /* Write final code byte */
    *code_ptr = code;

    return (uint32_t)(out - dst);
}

/* -------------------------------------------------------------------------
 * COBS decode
 * ------------------------------------------------------------------------- */
uint32_t hui_proto_cobs_decode(const uint8_t *src, uint32_t len, uint8_t *dst)
{
    const uint8_t *src_end = src + len;
    uint8_t       *out     = dst;

    while (src < src_end) {
        uint8_t code = *src++;
        if (code == 0) return 0; /* 0x00 is not valid inside a COBS frame */

        uint8_t i;
        for (i = 1; i < code; i++) {
            if (src >= src_end) return 0; /* truncated */
            *out++ = *src++;
        }

        /* If code < 0xFF and we are not at end, emit the implicit 0x00 */
        if (code < 0xFF && src < src_end) {
            *out++ = 0x00;
        }
    }

    return (uint32_t)(out - dst);
}

/* -------------------------------------------------------------------------
 * Length-prefix framing
 * ------------------------------------------------------------------------- */
uint32_t hui_proto_write_frame(FILE *fp, const uint8_t *payload,
                               uint32_t payload_len)
{
    uint8_t prefix[4];
    hui__proto_put_u32(prefix, payload_len);

    if (fwrite(prefix, 1, 4, fp) != 4)            return 0;
    if (fwrite(payload, 1, payload_len, fp) != payload_len) return 0;

    return 4u + payload_len;
}

uint32_t hui_proto_read_frame(FILE *fp, uint8_t *buf, uint32_t bufsz)
{
    uint8_t prefix[4];
    if (fread(prefix, 1, 4, fp) != 4) return 0;

    uint32_t payload_len = hui__proto_get_u32(prefix);
    if (payload_len == 0)           return 0;
    if (payload_len > bufsz)        return 0;

    if (fread(buf, 1, payload_len, fp) != payload_len) return 0;

    return payload_len;
}

/* -------------------------------------------------------------------------
 * Convenience wrappers
 * ------------------------------------------------------------------------- */

/* Forward-declared in hui.h: the global draw list pointer */
extern hui_dl *hui_dl_g;

bool hui_proto_flush_to_file(FILE *fp, uint32_t frame_seq)
{
    if (!hui_dl_g) return false;

    uint32_t encoded = hui_proto_encode(hui_dl_g, hui__proto_scratch,
                                        HUI_PROTO_SCRATCH_SZ, frame_seq);
    if (encoded == 0) return false; /* frame too large for scratch buffer */

    return hui_proto_write_frame(fp, hui__proto_scratch, encoded) != 0;
}

uint16_t hui_proto_replay_from_file(FILE *fp, hui_dl *dl_out)
{
    if (!dl_out) return 0;

    uint32_t payload_len = hui_proto_read_frame(fp, hui__proto_scratch,
                                                HUI_PROTO_SCRATCH_SZ);
    if (payload_len == 0) return 0;

    const hui_cmd  *cmds      = NULL;
    const char     *strpool   = NULL;
    const float    *datapool  = NULL;
    hui_proto_header hdr;

    uint16_t count = hui_proto_decode(hui__proto_scratch, payload_len,
                                      &cmds, &strpool, &datapool, &hdr);
    if (count == 0 && hdr.cmd_count == 0) return 0;

    /* Copy into dl_out — caller is responsible for sufficient capacity */
    uint32_t cmd_bytes      = (uint32_t)hdr.cmd_count * 32u;
    uint32_t strpool_bytes  = hdr.strpool_len;
    uint32_t datapool_bytes = hdr.datapool_len * 4u;

    memcpy(dl_out->cmds,     cmds,     cmd_bytes);
    memcpy(dl_out->strpool,  strpool,  strpool_bytes);
    memcpy(dl_out->datapool, datapool, datapool_bytes);

    dl_out->count        = hdr.cmd_count;
    dl_out->strpool_len  = (uint16_t)hdr.strpool_len;
    dl_out->datapool_len = hdr.datapool_len;

    return hdr.cmd_count;
}

#endif /* HUI_PROTO_IMPLEMENTATION */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* HUI_PROTO_H */
