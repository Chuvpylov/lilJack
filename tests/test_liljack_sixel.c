#define _GNU_SOURCE
#include "../liljack_app/c_sixel.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* ── a minimal sixel decoder, to prove the encoder round-trips ──────────── */
static uint32_t reg[256];
static int reg_set[256];

static void put_sixel(uint32_t *dst, int w, int h, int *col, int band, int color, int mask) {
    for (int row = 0; row < 6; row++) {
        if (!(mask & (1 << row))) continue;
        int py = band + row, px = *col;
        if (px >= 0 && px < w && py >= 0 && py < h && reg_set[color])
            dst[py * w + px] = reg[color];
    }
    (*col)++;
}

static void decode(const char *s, int n, uint32_t *dst, int w, int h) {
    memset(dst, 0, (size_t)w * h * 4);
    memset(reg_set, 0, sizeof reg_set);
    int i = 0;
    /* find DCS ESC P ... q */
    while (i + 1 < n && !(s[i] == 0x1b && s[i + 1] == 'P')) i++;
    i += 2;
    while (i < n && s[i] != 'q') i++;
    i++;                                   /* past q */
    int col = 0, band = 0, color = 0;
    while (i < n) {
        char c = s[i];
        if (c == 0x1b && i + 1 < n && s[i + 1] == '\\') break;   /* ST */
        if (c == '#') {
            i++;
            int num = 0;
            while (i < n && s[i] >= '0' && s[i] <= '9') num = num * 10 + (s[i++] - '0');
            if (i + 1 < n && s[i] == ';') {
                /* colour definition #n;2;r;g;b */
                int r = 0, g = 0, b = 0;
                int mode = 0;
                while (i < n && s[i] != '#' && s[i] != '$' && s[i] != '-' && s[i] != '"') {
                    if (s[i] >= '0' && s[i] <= '9') {
                        int v = 0;
                        while (i < n && s[i] >= '0' && s[i] <= '9') v = v * 10 + (s[i++] - '0');
                        if (mode == 2) r = v; else if (mode == 3) g = v; else if (mode == 4) b = v;
                    } else if (s[i] == ';') { mode++; i++; }
                    else i++;
                }
                reg[num] = (uint32_t)((r * 255 + 50) / 100) << 16
                         | (uint32_t)((g * 255 + 50) / 100) << 8
                         | (uint32_t)((b * 255 + 50) / 100);
                reg_set[num] = 1;
                continue;
            }
            color = num;
            continue;
        }
        if (c == '$') { col = 0; i++; continue; }
        if (c == '-') { band += 6; col = 0; i++; continue; }
        if (c == '"') { i++; continue; }                          /* raster attr */
        if (c == '!') {
            i++;
            int cnt = 0;
            while (i < n && s[i] >= '0' && s[i] <= '9') cnt = cnt * 10 + (s[i++] - '0');
            if (i < n) { int mask = s[i++] - 63; for (int k = 0; k < cnt; k++) put_sixel(dst, w, h, &col, band, color, mask); }
            continue;
        }
        if (c >= 63 && c <= 126) { i++; put_sixel(dst, w, h, &col, band, color, c - 63); continue; }
        i++;
    }
}

int main(void) {
    /* ── DA1 parser ────────────────────────────────────────────────────── */
    CHECK(lj_sixel_da1_has_sixel("\x1b[?1;2;4;6c", 10) == 1);
    CHECK(lj_sixel_da1_has_sixel("\x1b[?4c", 5) == 1);
    CHECK(lj_sixel_da1_has_sixel("\x1b[?62;4;22c", 11) == 1);
    CHECK(lj_sixel_da1_has_sixel("\x1b[?1;2c", 7) == 0);
    CHECK(lj_sixel_da1_has_sixel("garbage", 7) == 0);
    CHECK(lj_sixel_da1_has_sixel(NULL, 0) == 0);

    /* ── encode + decode round-trip ───────────────────────────────────── */
    enum { W = 10, H = 8 };
    uint32_t frame[W * H];
    for (int i = 0; i < W * H; i++) frame[i] = 0xFFCC0000;   /* red */
    for (int x = 0; x < W; x++) frame[x] = 0xFF00CC00;        /* top row green */
    frame[4 * W + 7] = 0xFF0000CC;                            /* one blue pixel */

    char out[4096];
    int n = lj_sixel_encode(frame, W, H, 0, 0, W, H, out, sizeof out);
    CHECK(n > 0 && n < (int)sizeof out);
    CHECK(strncmp(out, "\x1bP", 2) == 0);
    CHECK(n >= 2 && out[n - 1] == '\\' && out[n - 2] == 0x1b);   /* ends ESC \ */

    uint32_t back[W * H];
    decode(out, n, back, W, H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            CHECK((back[y * W + x] & 0x00FFFFFFu) == (frame[y * W + x] & 0x00FFFFFFu));

    /* ── damage rect: encode only a sub-rect ───────────────────────────── */
    n = lj_sixel_encode(frame, W, H, 3, 2, 4, 4, out, sizeof out);
    CHECK(n > 0);
    memset(back, 0, sizeof back);
    decode(out, n, back, 4, 4);   /* decode as a standalone 4x4 image */
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            CHECK((back[y * 4 + x] & 0x00FFFFFFu) == (frame[(y + 2) * W + (x + 3)] & 0x00FFFFFFu));

    /* ── quantisation: >256 distinct colours falls back to the fixed cube ── */
    {
        enum { GW = 20, GH = 20 };
        static uint32_t grad[GW * GH];
        for (int y = 0; y < GH; y++)
            for (int x = 0; x < GW; x++)
                grad[y * GW + x] = 0xFF000000u | ((x * 13) & 0xFF) << 16
                                 | ((y * 7) & 0xFF) << 8 | (((x + y) * 3) & 0xFF);
        char *big = (char *)malloc(65536);
        CHECK(big != NULL);
        int bn = lj_sixel_encode(grad, GW, GH, 0, 0, GW, GH, big, 65536);
        CHECK(bn > 0);                    /* encodes without overflow */
        static uint32_t gback[GW * GH];
        decode(big, bn, gback, GW, GH);
        int unfilled = 0;
        for (int i = 0; i < GW * GH; i++)
            if ((gback[i] & 0x00FFFFFFu) == 0) unfilled++;
        CHECK(unfilled == 0);             /* every pixel got a defined register */
        free(big);
    }

    /* ── size cap: a big frame into a tiny buffer must not overflow ────── */
    CHECK(lj_sixel_encode(frame, W, H, 0, 0, W, H, out, 4) == -1);
    CHECK(lj_sixel_encode(frame, W, H, 0, 0, W, H, out, 0) == -1);

    /* ── DA1 over a socketpair (the "terminal" side pre-supplies the reply) */
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    const char *yes = "\x1b[?6;4;2c";
    CHECK(write(sv[0], yes, (int)strlen(yes)) == (int)strlen(yes));
    CHECK(lj_sixel_supported(sv[1]) == 1);
    close(sv[0]); close(sv[1]);

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    const char *no = "\x1b[?6;2c";
    CHECK(write(sv[0], no, (int)strlen(no)) == (int)strlen(no));
    CHECK(lj_sixel_supported(sv[1]) == 0);
    close(sv[0]); close(sv[1]);

    /* ── real terminals' DA1 replies (kitty-terminal-matrix): kitty refuses
     * sixel and answers ESC[?62;c — lilJack must take the cell path there;
     * WezTerm answers with many attributes and 4 among them. */
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    const char *kitty = "\x1b[?62;52;c";     /* recorded from kitty 0.48 on :0, 2026-09-13 */
    CHECK(write(sv[0], kitty, (int)strlen(kitty)) == (int)strlen(kitty));
    CHECK(lj_sixel_supported(sv[1]) == 0);
    close(sv[0]); close(sv[1]);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    const char *wez = "\x1b[?65;4;6;18;22c";
    CHECK(write(sv[0], wez, (int)strlen(wez)) == (int)strlen(wez));
    CHECK(lj_sixel_supported(sv[1]) == 1);
    close(sv[0]); close(sv[1]);

    /* ── DA1 over a REAL pty: the raw-mode query must not eat the reply ─── */
    {
        int master = posix_openpt(O_RDWR | O_NOCTTY);
        if (master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0) {
            char *name = ptsname(master);
            int slave = name ? open(name, O_RDWR | O_NOCTTY) : -1;
            if (slave >= 0) {
                pid_t pid = fork();
                if (pid == 0) {
                    close(slave);
                    char q[16];
                    ssize_t k = read(master, q, sizeof q);   /* the DA1 query */
                    CHECK(k >= 3 && q[0] == 0x1b && q[1] == '[');
                    const char *resp = "\x1b[?6;4;2c";
                    CHECK(write(master, resp, (int)strlen(resp)) == (int)strlen(resp));
                    /* ⚠ HOLD THE MASTER OPEN UNTIL THE PARENT HAS READ. On Linux a
                     * read on the slave returns EIO the moment the LAST master fd
                     * closes, so closing immediately after write raced the reply
                     * away and the detector was blamed for a test that discarded
                     * its own answer. The detector polls for at most ~200ms. */
                    struct timespec hold = { 0, 400L * 1000 * 1000 };
                    nanosleep(&hold, NULL);
                    close(master);
                    _exit(0);
                }
                close(master);
                int ok = lj_sixel_supported(slave);
                int status = 0;
                CHECK(waitpid(pid, &status, 0) == pid);
                close(slave);
                CHECK(ok == 1);    /* sixel detected through a real tty */
            } else if (slave < 0) {
                close(master);
            }
        } else if (master >= 0) {
            close(master);
        }
    }

    printf("C sixel: DA1 parser, encode/decode round-trip, damage rect, size cap, "
           "socketpair + pty DA1 passed");
    printf(" · full 10x8 frame = %d bytes\n", lj_sixel_encode(frame, W, H, 0, 0, W, H, out, sizeof out));
    return 0;
}
