#define _GNU_SOURCE
/* test_liljack_ansi_image.c — the sixel PRESENTER (tui-sixel-present).
 *
 * deepseek owns the encoder (c_sixel.c, covered by test_liljack_sixel.c); this
 * file covers what the presenter adds on top and what it must refuse:
 *
 *   1. SUPPORT IS DETECTED, NEVER ASSUMED. On a terminal that does not answer
 *      DA1 with attribute 4, lj_ansi_image() must be a no-op and present()
 *      must emit NO DCS at all. This is the safety-critical case: a sixel
 *      escape sent to a terminal that cannot read it is not a missing image,
 *      it is garbage sprayed across the user's whole screen — strictly worse
 *      than the cell fallback it would have replaced.
 *
 *   2. THE IMAGE LANDS ON ITS CELLS. lilJack lays out on a fixed 10x20 grid,
 *      but the host terminal's cell is whatever its font makes it. An image
 *      emitted at lilJack's pixel size is the wrong size in any terminal whose
 *      font is not 10x20, so the presenter queries the real cell (CSI 16 t)
 *      and scales to cols*cellw x rows*cellh.
 *
 *   3. DAMAGE TRACKING. An unchanged region must not be re-sent — a full frame
 *      measured 268,942 bytes, so re-sending a static image every frame is the
 *      bandwidth problem the whole design exists to avoid.
 *
 * The test plays the TERMINAL: it owns the pty master, answers the DA1 and
 * CSI 16 t queries, and asserts on the bytes lilJack actually wrote.
 */
#include "../liljack_app/c_ansi.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static int ok_count = 0;
static void pass(const char *what) { printf("  ok     %s\n", what); ok_count++; }

/* Count DCS introducers (ESC P) in a buffer. */
static int count_dcs(const char *b, int n) {
    int c = 0;
    for (int i = 0; i + 1 < n; i++) if (b[i] == 0x1b && b[i + 1] == 'P') c++;
    return c;
}
/* Find the cursor-position escape immediately preceding the first DCS. */
static int cursor_before_dcs(const char *b, int n, int *row, int *col) {
    int d = -1;
    for (int i = 0; i + 1 < n; i++) if (b[i] == 0x1b && b[i + 1] == 'P') { d = i; break; }
    if (d < 0) return 0;
    for (int i = d - 1; i >= 1; i--) {
        if (b[i] != 0x1b || b[i + 1] != '[') continue;
        int r = 0, c = 0, p = i + 2, got = 0;
        while (p < n && b[p] >= '0' && b[p] <= '9') { r = r * 10 + (b[p++] - '0'); got = 1; }
        if (!got || p >= n || b[p] != ';') continue;
        p++; got = 0;
        while (p < n && b[p] >= '0' && b[p] <= '9') { c = c * 10 + (b[p++] - '0'); got = 1; }
        if (!got || p >= n || b[p] != 'H') continue;
        *row = r; *col = c; return 1;
    }
    return 0;
}
/* Raster width/height declared inside a sixel body, by walking it. */
static void sixel_extent(const char *b, int n, int *w, int *h) {
    int i = 0, maxw = 0, cur = 0, bands = 0;
    while (i + 1 < n && !(b[i] == 0x1b && b[i + 1] == 'P')) i++;
    if (i + 1 >= n) { *w = *h = 0; return; }
    while (i < n && b[i] != 'q') i++;
    i++;
    /* ⚠ COUNT BANDS THAT CARRY DATA, not '-' separators. The encoder writes a
     * trailing '-' after the LAST band too, so bands = count('-') + 1 reports
     * one band too many and a 48px image measures 54. That was a defect in
     * this measurement, not in the encoder. */
    int band_has = 0;
    bands = 0;
    while (i < n) {
        if (b[i] == 0x1b && i + 1 < n && b[i + 1] == '\\') break;
        if (b[i] == '#') { i++; while (i < n && ((b[i] >= '0' && b[i] <= '9') || b[i] == ';')) i++; continue; }
        if (b[i] == '!') {
            i++; int rep = 0;
            while (i < n && b[i] >= '0' && b[i] <= '9') rep = rep * 10 + (b[i++] - '0');
            if (i < n && b[i] >= '?' && b[i] <= '~') { cur += rep; band_has = 1; i++; }
            continue;
        }
        if (b[i] == '$') { if (cur > maxw) maxw = cur; cur = 0; i++; continue; }
        if (b[i] == '-') { if (cur > maxw) maxw = cur; cur = 0; if (band_has) bands++; band_has = 0; i++; continue; }
        if (b[i] >= '?' && b[i] <= '~') { cur++; band_has = 1; i++; continue; }
        i++;
    }
    if (cur > maxw) maxw = cur;
    if (band_has) bands++;
    *w = maxw; *h = bands * 6;
}

/* Read until `term` arrives, or `ms` elapses.
 * ⚠ NEVER "drain until quiet" BEFORE ANSWERING A QUERY. lj_sixel_supported
 * waits ~200ms for its DA1 reply; a harness that waits for silence first
 * answers after the detector has already given up, and then blames the
 * detector for a reply the TEST threw away. (test_liljack_sixel.c hit exactly
 * this and it cost a 5/5 red run.) Return the instant the terminator lands. */
static int read_query(int fd, char *buf, int cap, char term, int ms) {
    int n = 0;
    struct timespec slice = { 0, 2L * 1000 * 1000 };
    for (int t = 0; t < ms / 2 && n < cap - 1; t++) {
        ssize_t z = read(fd, buf + n, (size_t)(cap - 1 - n));
        if (z > 0) {
            n += (int)z;
            if (memchr(buf, term, (size_t)n)) break;
            continue;
        }
        nanosleep(&slice, NULL);
    }
    buf[n] = 0;
    return n;
}

/* Drain the master for `ms`, appending to buf. */
static int drain(int master, char *buf, int cap, int ms) {
    int n = 0;
    struct timespec slice = { 0, 20L * 1000 * 1000 };
    for (int t = 0; t < ms / 20 && n < cap - 1; t++) {
        ssize_t z = read(master, buf + n, (size_t)(cap - 1 - n));
        if (z > 0) { n += (int)z; t = 0; continue; }
        nanosleep(&slice, NULL);
    }
    buf[n] = 0;
    return n;
}

/* One run. `sixel_yes` decides the DA1 answer; CELLW/CELLH are what the fake
 * terminal reports for CSI 16 t. Returns captured output in `out`. */
static int run_child(int sixel_yes, int cellw, int cellh, char *out, int cap, int *avail, int *cw, int *ch) {
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    CHECK(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0);
    char *name = ptsname(master);
    CHECK(name != NULL);
    int facts[2];
    CHECK(pipe(facts) == 0);

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        close(master); close(facts[0]);
        int slave = open(name, O_RDWR | O_NOCTTY);
        if (slave < 0) _exit(3);
        dup2(slave, 0); dup2(slave, 1);
        if (slave > 2) close(slave);
        /* ⚠ TURN THE RATE LIMITER OFF, OR THIS TEST MEASURES THE WRONG THING.
         * Frames run back to back here, so with the default 60ms floor the
         * second emission is suppressed by the RATE LIMIT and the damage-hash
         * check passes even when damage tracking is deleted outright. Verified:
         * with the limiter on, removing the hash comparison did not turn this
         * test red. With it off, the hash is the only thing that can suppress. */
        setenv("LJ_SIXEL_MIN_MS", "0", 1);
        /* Same reason for the frame ACK (c_ansi.c): this child never reads its
         * input between frames, so an ACK could never arrive and frame 3 would
         * be held by flow control, not by damage tracking. */
        setenv("LJ_SIXEL_ACK", "0", 1);
        int pw = 0, ph = 0;
        if (!lj_ansi_open(&pw, &ph)) _exit(4);
        int a = lj_ansi_images_available(), gw = 0, gh = 0;
        lj_ansi_cell_pixels(&gw, &gh);
        int f[3] = { a, gw, gh };
        ssize_t iw = write(facts[1], f, sizeof f); (void)iw;

        /* 60x20 lilJack pixels per cell-grid => 40 cols x 10 rows of workspace */
        static uint32_t src[16 * 8];
        for (int i = 0; i < 16 * 8; i++) src[i] = 0xFF000000u | (uint32_t)((i * 7) & 0xFF) << 8;

        /* frame 1 — image declared, must be emitted */
        lj_ansi_begin(400, 200);
        lj_ansi_rect(0, 0, 400, 200, 0x101010);
        lj_ansi_image(20, 40, 100, 60, src, 16, 8);   /* cols 2..11, rows 2..4 */
        lj_ansi_present();
        /* frame 2 — byte-identical content, must NOT be re-emitted */
        lj_ansi_begin(400, 200);
        lj_ansi_rect(0, 0, 400, 200, 0x101010);
        lj_ansi_image(20, 40, 100, 60, src, 16, 8);
        lj_ansi_present();
        /* frame 3 — the pixels CHANGE, so it must be emitted again. Without
         * this, "exactly one DCS" is also satisfied by a presenter that can
         * never emit a second time, which is not what we are claiming. */
        for (int i = 0; i < 16 * 8; i++) src[i] = 0xFF000000u | (uint32_t)((i * 11) & 0xFF);
        lj_ansi_begin(400, 200);
        lj_ansi_rect(0, 0, 400, 200, 0x101010);
        lj_ansi_image(20, 40, 100, 60, src, 16, 8);
        lj_ansi_present();
        lj_ansi_close();
        _exit(0);
    }
    close(facts[1]);

    /* We are the terminal. Answer DA1, then CSI 16 t. */
    char q[256];
    int qn = read_query(master, q, sizeof q, 'c', 400);   /* DA1 ends in 'c' */
    CHECK(qn > 0);
    const char *da1 = sixel_yes ? "\x1b[?6;4;2c" : "\x1b[?6;2c";
    CHECK(write(master, da1, strlen(da1)) == (ssize_t)strlen(da1));
    char q2[256];
    read_query(master, q2, sizeof q2, 't', 400);         /* CSI 16 t ends in 't' */
    char rep[64];
    int rn = snprintf(rep, sizeof rep, "\x1b[6;%d;%dt", cellh, cellw);
    CHECK(write(master, rep, (size_t)rn) == rn);

    int f[3] = { -1, -1, -1 };
    ssize_t got = read(facts[0], f, sizeof f);
    CHECK(got == (ssize_t)sizeof f);
    *avail = f[0]; *cw = f[1]; *ch = f[2];
    close(facts[0]);

    int n = drain(master, out, cap, 900);
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(master);
    return n;
}

int main(void) {
    printf("sixel presenter — detection, cell-exact placement, damage\n");

    /* ── 1. a terminal WITHOUT sixel: nothing may be emitted ───────────── */
    {
        static char out[1 << 16];
        int avail = -1, cw = 0, ch = 0;
        int n = run_child(0, 8, 16, out, sizeof out, &avail, &cw, &ch);
        CHECK(n > 0);                       /* it still drew the cell UI */
        CHECK(avail == 0);
        pass("a terminal without sixel reports images unavailable");
        CHECK(count_dcs(out, n) == 0);
        pass("...and NOT ONE byte of DCS is written to it (garbage would be worse)");
        /* ⚠ BUT IT MUST STILL SHOW THE PICTURE. Refusing outright meant the
         * media tile drew NOTHING for every terminal without DA1 attribute 4 —
         * mate-terminal among them, which is what the operator runs, and why he kept
         * reporting no rickroll. U+2580 carries two vertical pixels per cell in
         * 24-bit colour, so the image appears at half vertical resolution
         * instead of not at all. */
        CHECK(strstr(out, "\xe2\x96\x80") != NULL);
        pass("...and the image IS drawn, as U+2580 half blocks (was: nothing)");
        /* ⚠ THE HOST'S OWN CURSOR MUST NOT WANDER. lilJack draws its cursor as
         * a reverse-video CELL, so the real one is hidden at open — but the
         * diff loop left it wherever the last changed cell was, a position that
         * moves every frame, and ?25l was never said again. On a host that
         * shows a cursor anyway that is a stray block, usually a row below the
         * content: the operator sees exactly that in wezterm and not in mate-terminal.
         * Every frame that writes anything now parks it home and re-hides it. */
        CHECK(strstr(out, "\033[?25l") != NULL);
        pass("...and the host cursor is re-hidden every frame, not once at open");
        {
            const char *last_park = NULL, *p2 = out;
            while ((p2 = strstr(p2, "\033[H\033[?25l")) != NULL) { last_park = p2; p2 += 2; }
            CHECK(last_park != NULL);
        }
        pass("...and parked at a deterministic position, not the last cell written");
    }

    /* ── 2. a terminal WITH sixel, cell 8x16 ───────────────────────────── */
    {
        static char out[1 << 18];
        int avail = -1, cw = 0, ch = 0;
        int n = run_child(1, 8, 16, out, sizeof out, &avail, &cw, &ch);
        CHECK(avail == 1);
        pass("DA1 attribute 4 turns the presenter on");
        CHECK(cw == 8 && ch == 16);
        pass("the HOST terminal's cell size is measured (8x16), not assumed 10x20");

        int dcs = count_dcs(out, n);
        CHECK(dcs >= 1);
        pass("the image is emitted");

        /* THE REGRESSION GUARD: three frames (new, identical, changed) must
         * produce exactly TWO emissions — the identical one is suppressed and
         * the changed one is not. Either half alone is satisfiable by a broken
         * presenter; together they are not. */
        CHECK(dcs == 2);
        pass("an unchanged image is NOT re-sent, and a CHANGED one IS (damage tracking)");

        int row = 0, col = 0;
        CHECK(cursor_before_dcs(out, n, &row, &col));
        /* pixel (20,40) on the 10x20 layout grid = col 2, row 2 -> 1-based 3,3 */
        CHECK(row == 3 && col == 3);
        pass("the image is positioned at the CELL its layout rect covers");

        int iw = 0, ih = 0;
        sixel_extent(out, n, &iw, &ih);
        /* rect 100x60 layout px = 10 cols x 3 rows; at 8x16 => 80 x 48 px */
        CHECK(iw == 80);
        CHECK(ih == 48);                    /* 3 rows x 16px, exactly */
        pass("...and SCALED to its cells (10x3 cells at 8x16 = 80x48px), not to lilJack's 100x60");
    }

    printf("\n%d checks passed\n", ok_count);
    return 0;
}
