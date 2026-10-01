#define _GNU_SOURCE
#include "c_sixel.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define MAXC 256          /* sixel colour registers are 0..255 */
#define HN   1024         /* open-addressing hash slots (power of two) */

/* ── output writer with a hard cap ─────────────────────────────────────── */
typedef struct { char *out; int cap; int n; int overflow; } W;
static void w_bytes(W *w, const char *s, int len) {
    if (w->overflow) return;
    if (w->n + len >= w->cap) { w->overflow = 1; return; }  /* leave room for NUL */
    memcpy(w->out + w->n, s, (size_t)len);
    w->n += len;
}
static void w_str(W *w, const char *s) { w_bytes(w, s, (int)strlen(s)); }
static void w_char(W *w, char c) { w_bytes(w, &c, 1); }
static void w_num(W *w, int v) { char b[16]; int n = snprintf(b, sizeof b, "%d", v); w_bytes(w, b, n); }

/* ── colour quantisation ───────────────────────────────────────────────── */
/* Exact distinct colours up to 256; beyond that a fixed 3:3:2 cube. */
static uint32_t fixed_of(uint32_t rgb) {
    return (uint32_t)((((rgb >> 16) & 0xFF) >> 5) << 5
                    | (((rgb >> 8) & 0xFF) >> 5) << 2
                    | ((rgb & 0xFF) >> 6));
}
static uint32_t rgb_of_fixed(uint32_t idx) {
    uint32_t r = (((idx >> 5) & 7) << 5) + 16;
    uint32_t g = (((idx >> 2) & 7) << 5) + 16;
    uint32_t b = ((idx & 3) << 6) + 32;
    return (r << 16) | (g << 8) | b;
}
static uint32_t hash24(uint32_t rgb) {
    uint32_t k = rgb ^ (rgb >> 16);
    k *= 0x85ebca6bu;
    k ^= k >> 13;
    return k & (HN - 1);
}

static uint32_t seen_rgb[HN];
static uint8_t  seen_occ[HN];
static uint32_t pal_rgb[MAXC];
static uint8_t  slot_of[HN];    /* rgb -> palette slot (exact mode) */
static uint8_t  used[MAXC];     /* fixed mode: registers actually present */
static int      ncolors, fixed_mode;

static uint32_t pixel_rgb(const uint32_t *frame, int fw, int x, int y) {
    return frame[(size_t)y * fw + x] & 0x00FFFFFFu;
}

/* One pass: count distinct colours; stop early once 256 is exceeded. */
static int count_distinct(const uint32_t *frame, int fw, int x, int y, int w, int h) {
    memset(seen_occ, 0, sizeof seen_occ);
    int count = 0;
    for (int yy = 0; yy < h && count <= MAXC; yy++) {
        for (int xx = 0; xx < w && count <= MAXC; xx++) {
            uint32_t rgb = pixel_rgb(frame, fw, x + xx, y + yy);
            uint32_t hh = hash24(rgb);
            while (seen_occ[hh] && seen_rgb[hh] != rgb) hh = (hh + 1) & (HN - 1);
            if (!seen_occ[hh]) { seen_occ[hh] = 1; seen_rgb[hh] = rgb; count++; }
        }
    }
    return count;
}

/* Build the exact palette (rgb -> slot) from the seen set of `count` colours. */
static void build_exact(int count) {
    memset(slot_of, 0xFF, sizeof slot_of);
    ncolors = 0;
    for (uint32_t hh = 0; hh < HN && ncolors < count; hh++) {
        if (!seen_occ[hh]) continue;
        uint32_t rgb = seen_rgb[hh];
        pal_rgb[ncolors] = rgb;
        uint32_t slot = hash24(rgb);
        while (slot_of[slot] != 0xFF) slot = (slot + 1) & (HN - 1);
        slot_of[slot] = (uint8_t)ncolors;
        ncolors++;
    }
}

static uint8_t exact_slot(uint32_t rgb) {
    uint32_t hh = hash24(rgb);
    while (slot_of[hh] != 0xFF && pal_rgb[slot_of[hh]] != rgb) hh = (hh + 1) & (HN - 1);
    return slot_of[hh];   /* must be present: the pixel was seen in pass 1 */
}

/* Fill the per-band index row for a single band of `bh` rows. */
static void band_index(const uint32_t *frame, int fw, int x, int y0, int w, int bh,
                       uint8_t *idx) {
    for (int row = 0; row < bh; row++) {
        for (int col = 0; col < w; col++) {
            uint32_t rgb = pixel_rgb(frame, fw, x + col, y0 + row);
            idx[row * w + col] = fixed_mode ? (uint8_t)fixed_of(rgb) : exact_slot(rgb);
        }
    }
}

static void emit_run(W *w, int mask, int len) {
    char ch = (char)(63 + mask);
    if (len <= 3) { for (int i = 0; i < len; i++) w_char(w, ch); }
    else { w_char(w, '!'); w_num(w, len); w_char(w, ch); }
}

static void emit_palette(W *w) {
    for (int c = 0; c < MAXC; c++) {
        uint32_t rgb;
        if (fixed_mode) {
            if (!used[c]) continue;
            rgb = rgb_of_fixed((uint32_t)c);
        } else {
            if (c >= ncolors) break;
            rgb = pal_rgb[c];
        }
        w_char(w, '#');
        w_num(w, c);
        w_str(w, ";2;");
        w_num(w, (int)((((rgb >> 16) & 0xFF) * 100) / 255));
        w_char(w, ';');
        w_num(w, (int)((((rgb >> 8) & 0xFF) * 100) / 255));
        w_char(w, ';');
        w_num(w, (int)(((rgb & 0xFF) * 100) / 255));
    }
}

int lj_sixel_encode(const uint32_t *frame, int fw, int fh,
                    int x, int y, int w, int h, char *out, int cap) {
    (void)fh;
    if (!frame || !out || cap <= 0) return -1;
    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > fw || y + h > fh) return -1;

    W wb = { out, cap, 0, 0 };

    int count = count_distinct(frame, fw, x, y, w, h);
    fixed_mode = (count > MAXC);
    if (fixed_mode) {
        memset(used, 0, sizeof used);
        for (int yy = 0; yy < h; yy++)
            for (int xx = 0; xx < w; xx++)
                used[fixed_of(pixel_rgb(frame, fw, x + xx, y + yy))] = 1;
    } else {
        build_exact(count);
    }

    w_str(&wb, "\x1bP0;0;0q");
    emit_palette(&wb);

    uint8_t *idx = (uint8_t *)malloc((size_t)w * 6);
    if (!idx) return -1;
    int bands = (h + 5) / 6;
    for (int b = 0; b < bands && !wb.overflow; b++) {
        int y0 = y + b * 6;
        int bh = (y0 + 6 <= y + h) ? 6 : (y + h - y0);
        band_index(frame, fw, x, y0, w, bh, idx);

        uint8_t band_used[MAXC];
        memset(band_used, 0, sizeof band_used);
        for (int i = 0; i < w * bh; i++) band_used[idx[i]] = 1;

        for (int c = 0; c < MAXC && !wb.overflow; c++) {
            if (!band_used[c]) continue;
            w_char(&wb, '#');
            w_num(&wb, c);
            /* Every column advances the sixel cursor, blank ones included, so a
             * sparse colour pass emits `?` (mask 0) to reach its columns. RLE
             * spans identical masks, blank runs included. */
            int prev = -1, run = 0;
            for (int col = 0; col < w; col++) {
                int mask = 0;
                for (int row = 0; row < bh; row++)
                    if (idx[row * w + col] == (uint8_t)c) mask |= (1 << row);
                if (mask == prev) run++;
                else { if (run) emit_run(&wb, prev, run); prev = mask; run = 1; }
            }
            if (run) emit_run(&wb, prev, run);
            w_char(&wb, '$');
        }
        w_char(&wb, '-');
    }
    free(idx);

    if (wb.overflow) return -1;
    w_str(&wb, "\x1b\\");
    out[wb.n] = '\0';
    return wb.n;
}

int lj_sixel_da1_has_sixel(const char *response, int len) {
    if (!response || len <= 0) return 0;
    int i = 0;
    while (i + 1 < len) {
        if (response[i] == 0x1b && response[i + 1] == '[') { i += 2; break; }
        if ((unsigned char)response[i] == 0x9b) { i += 1; break; }
        i++;
    }
    if (i >= len) return 0;
    if (i < len && (response[i] == '?' || response[i] == '>' ||
                    response[i] == '=' || response[i] == '<')) i++;
    int num = -1;
    while (i < len && response[i] != 'c') {
        char ch = response[i];
        if (ch >= '0' && ch <= '9') {
            if (num < 0) num = 0;
            num = num * 10 + (ch - '0');
        } else if (ch == ';') {
            if (num == 4) return 1;
            num = -1;
        }
        i++;
    }
    return num == 4;
}

int lj_sixel_supported(int fd) {
    if (fd < 0) return 0;
    /* ⚠ DA1 MUST BE QUERIED IN RAW MODE. In canonical mode the reply carries no
     * newline, so the line discipline buffers it and read() never sees it, and
     * ECHO paints it onto the screen — a false negative while the pane prints
     * ESC[?1;2;4c. Save the termios, cfmakeraw, query, then restore. */
    int have_tty = 0;
    struct termios saved, raw;
    if (tcgetattr(fd, &saved) == 0) {
        have_tty = 1;
        raw = saved;
        cfmakeraw(&raw);   /* leaves VMIN=1,VTIME=0: read blocks until a byte,
                              which is what we want once poll() says POLLIN */
        tcsetattr(fd, TCSANOW, &raw);
    }
    int result = 0;
    static const char da1[] = "\x1b[c";
    if (write(fd, da1, 3) == 3) {
        char buf[512];
        int n = 0;
        struct pollfd p = { .fd = fd, .events = POLLIN };
        for (int tries = 0; tries < 20; tries++) {
            if (poll(&p, 1, 10) <= 0) break;
            if (!(p.revents & POLLIN)) break;    /* HUP/ERR, not data */
            ssize_t got = read(fd, buf + n, sizeof buf - 1 - n);
            if (got <= 0) break;
            n += (int)got;
            buf[n] = '\0';
            if (memchr(buf, 'c', (size_t)n)) break;
            if (n >= (int)sizeof buf - 1) break;
        }
        result = lj_sixel_da1_has_sixel(buf, n);
    }
    if (have_tty) tcsetattr(fd, TCSANOW, &saved);
    if (result) return 1;
    /* ⚠ SOME TERMINALS RENDER SIXEL BUT DO NOT ADVERTISE IT IN DA1. WezTerm is
     * the live case: it plays sixel correctly, yet its DA1 reply (and its
     * terminfo, which has no Sxl) omit the sixel attribute, so the probe above
     * says "no" and every image falls back to nothing — the rickroll had
     * nowhere to render. Trust the host identity for the terminals known to do
     * this, so images actually show where they can. */
    {
        const char *tp = getenv("TERM_PROGRAM");
        const char *term = getenv("TERM");
        if (tp && (!strcmp(tp, "WezTerm") || !strcmp(tp, "mintty"))) return 1;
        if (term && (!strncmp(term, "wezterm", 7) || !strncmp(term, "mlterm", 6)
                     || !strncmp(term, "kitty", 5))) return 1;
    }
    return 0;
}
