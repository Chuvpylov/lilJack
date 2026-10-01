/* Synchronized output (DECSET 2026) — the receiver half of ansi-graphs-unsynced.
 * A presenter that shows the grid only while lj_vt_state(LJ_VT_SYNC)==0 must never
 * observe a partial frame, whichever byte the pty read boundary lands on.
 * Build: gcc -std=c11 -Wall -Wextra -I liljack_app tests/test_liljack_vt_sync.c liljack_app/owkterm_vt.c -o vt-sync && ./vt-sync */
#include "owkterm_vt.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CW 10
#define CH 20
#define NT 256
#define COLS 40
#define ROWS 4
static uint32_t tiles[NT][CW*CH]; static uint32_t next_tile = 1;
static void tile_reset(void *u, uint32_t id, uint32_t bg) { (void)u; for (int i = 0; i < CW*CH; i++) tiles[id][i] = bg; }
static uint32_t tile_new(void *u, uint32_t bg) { (void)u; uint32_t id = next_tile++; assert(id < NT); for (int i = 0; i < CW*CH; i++) tiles[id][i] = bg; return id; }
static void tile_plot(void *u, uint32_t id, int px, int py, uint32_t rgb) { (void)u; assert(id && id < NT); tiles[id][py*CW+px] = rgb; }
static void image_end(void *u) { (void)u; }
/* What a presenter would paint: every cell's glyph, and the pixel at (0,0) of its tile if any. */
typedef struct { uint32_t ch, px; } shot_cell;
static void snapshot(void *vt, shot_cell *out) {
    for (int y = 0; y < ROWS; y++) { const lj_cell *r = lj_vt_row(vt, y);
        for (int x = 0; x < COLS; x++) { out[y*COLS+x].ch = r[x].ch; out[y*COLS+x].px = r[x].marks[1] ? tiles[r[x].marks[1]][0] : 0; } }
}
static int same(const shot_cell *a, const shot_cell *b) { return !memcmp(a, b, sizeof(shot_cell) * COLS * ROWS); }
/* One lilJack-shaped header frame: fence, home, label, three 30 px images, fence. */
static int frame(char *buf, size_t cap, const char *label, const char *colour) {
    int n = snprintf(buf, cap, "\033[?2026h\033[H%s", label);
    for (int i = 0; i < 3; i++) n += snprintf(buf + n, cap - (size_t)n, "\033[1;%dH\033Pq#0;2;%s#0!30~-#0!30~-\033\\", 12 + i * 4, colour);
    n += snprintf(buf + n, cap - (size_t)n, "\033[?2026l");
    return n;
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    char oldf[1024], newf[1024]; int oldn = frame(oldf, sizeof oldf, "GPU 34%", "100;0;0"), newn = frame(newf, sizeof newf, "GPU 71%", "0;0;100");
    shot_cell before[COLS*ROWS], after[COLS*ROWS], seen[COLS*ROWS];
    int cuts = 0, partial = 0, first_bad = -1, presented_inside = 0;
    for (int cut = 1; cut < newn; cut++) {
        void *vt = lj_vt_new(COLS, ROWS); assert(vt); next_tile = 1;
        lj_vt_pixel_sink sink = { NULL, CW, CH, tile_new, tile_plot, tile_reset, image_end }; lj_vt_set_pixel_sink(vt, &sink);
        lj_vt_feed(vt, (const unsigned char *)oldf, oldn); assert(!lj_vt_state(vt, LJ_VT_SYNC)); snapshot(vt, before);
        lj_vt_feed(vt, (const unsigned char *)newf, cut);
        if (!lj_vt_state(vt, LJ_VT_SYNC)) {                       /* presenter would paint now */
            snapshot(vt, seen); presented_inside++;
            if (!same(seen, before)) { partial++; if (first_bad < 0) first_bad = cut; }
        }
        lj_vt_feed(vt, (const unsigned char *)newf + cut, newn - cut);
        assert(!lj_vt_state(vt, LJ_VT_SYNC)); snapshot(vt, after); assert(!same(after, before));
        cuts++; lj_vt_free(vt);
    }
    printf("vt-sync: frame bytes old=%d new=%d; cut points=%d; presenter paints inside the fence=%d; partial frames visible=%d; first partial cut=%d\n",
           oldn, newn, cuts, presented_inside, partial, first_bad);
    /* the fence must not leak into the grid as text */
    { void *vt = lj_vt_new(COLS, ROWS); lj_vt_feed(vt, (const unsigned char *)oldf, oldn);
      assert(lj_vt_row(vt, 0)[0].ch == 'G'); lj_vt_free(vt); }
    /* DECRST alone releases; a second DECSET while set stays set */
    { void *vt = lj_vt_new(COLS, ROWS); lj_vt_feed(vt, (const unsigned char *)"\033[?2026h\033[?2026h", 16);
      assert(lj_vt_state(vt, LJ_VT_SYNC) == 1); lj_vt_feed(vt, (const unsigned char *)"\033[?2026l", 8); assert(!lj_vt_state(vt, LJ_VT_SYNC)); lj_vt_free(vt); }
    /* paints before the 8-byte fence opener completes see the untouched old frame: allowed */
    if (partial) { printf("vt-sync: FAIL\n"); return 1; }
    printf("vt-sync: PASS\n"); return 0;
}
