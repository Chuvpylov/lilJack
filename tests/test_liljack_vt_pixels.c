/* VT pixel-level sixel sink (owkterm_vt.h lj_vt_pixel_sink).
 * Build: gcc -std=c11 -Wall -Wextra -I liljack_app tests/test_liljack_vt_pixels.c liljack_app/owkterm_vt.c -o vt-pixels && ./vt-pixels */
#include "owkterm_vt.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define CW 10
#define CH 20
#define NT 64
static uint32_t tiles[NT][CW*CH]; static uint32_t next_tile = 1; static uint32_t last_bg = 0xdeadbeef;
static void tile_reset(void *u, uint32_t id, uint32_t bg) { (void)u; for (int i = 0; i < CW*CH; i++) tiles[id][i] = 0xbb0000 | bg; }
static uint32_t seen[64]; static int nseen;
static void walk(void *u, uint32_t id) { (void)u; if (nseen < 64) seen[nseen++] = id; }
static int image_ends;
static void image_end(void *u) { (void)u; image_ends++; }
static uint32_t tile_new(void *u, uint32_t bg) { (void)u; last_bg = bg; uint32_t id = next_tile++; assert(id < NT); for (int i = 0; i < CW*CH; i++) tiles[id][i] = 0xbb0000 | bg; return id; }
static void tile_plot(void *u, uint32_t id, int px, int py, uint32_t rgb) { (void)u; assert(id && id < NT); assert(px >= 0 && px < CW && py >= 0 && py < CH); tiles[id][py*CW+px] = rgb; }
static void feed(void *vt, const char *s) { lj_vt_feed(vt, (const unsigned char *)s, (int)strlen(s)); }

int main(void) {
    void *vt = lj_vt_new(40, 10);
    /* no sink: DA1 is the plain VT100 answer, sixel makes half blocks */
    feed(vt, "\033[c"); assert(!strcmp(lj_vt_reply(vt), "\033[?1;2c")); lj_vt_clear_reply(vt);
    feed(vt, "\033Pq#0;2;100;0;0#0!4~\033\\");
    assert(lj_vt_row(vt, 0)[0].ch == 0x2580 && !(lj_vt_row(vt, 0)[0].fg & LJ_VT_FG_PIXELS));
    assert(lj_vt_state(vt, LJ_VT_CY) == 0);            /* half-block mode keeps the cursor */
    lj_vt_free(vt);

    vt = lj_vt_new(40, 10);
    lj_vt_pixel_sink sink = { NULL, CW, CH, tile_new, tile_plot, tile_reset, image_end };
    lj_vt_set_pixel_sink(vt, &sink);
    feed(vt, "\033[c"); assert(!strcmp(lj_vt_reply(vt), "\033[?62;4;22c")); lj_vt_clear_reply(vt);
    /* XTWINOPS: with a sink the VT reports real pixel geometry (lilJack's cell probe asks 16 t) */
    { char want[64];
      feed(vt, "\033[16t"); snprintf(want, sizeof want, "\033[6;%d;%dt", CH, CW); assert(!strcmp(lj_vt_reply(vt), want)); lj_vt_clear_reply(vt);
      feed(vt, "\033[14t"); snprintf(want, sizeof want, "\033[4;%d;%dt", 10 * CH, 40 * CW); assert(!strcmp(lj_vt_reply(vt), want)); lj_vt_clear_reply(vt);
      feed(vt, "\033[18t"); assert(!strcmp(lj_vt_reply(vt), "\033[8;10;40t")); lj_vt_clear_reply(vt); }
    /* 30 px wide, 24 px tall (4 sixel bands): red top half, blue bottom half, at cursor (col 2,row 1) */
    feed(vt, "\033[2;3H\033[44m");
    feed(vt, "\033Pq#0;2;100;0;0#1;2;0;0;100#0!30~-#0!30~-#1!30~-#1!30~-\033\\");
    const lj_cell *r1 = lj_vt_row(vt, 1);
    assert(r1[2].ch == 0x2580 && (r1[2].fg & LJ_VT_FG_PIXELS) && r1[2].marks[1]);
    assert(r1[4].ch == 0x2580 && r1[4].marks[1]);        /* 30 px = cols 2,3,4 */
    assert(!(r1[5].fg & LJ_VT_FG_PIXELS));               /* col 5 untouched */
    assert(last_bg == 4);                                /* raw bg index passed through (SGR 44) */
    uint32_t id = r1[2].marks[1];
    assert(tiles[id][0] == 0xff0000);                    /* (0,0) red */
    assert(tiles[id][11*CW + 0] == 0xff0000);            /* row 11 still red */
    assert(tiles[id][12*CW + 0] == 0x0000ff);            /* row 12 blue */
    assert(tiles[id][19*CW + 9] == 0x0000ff);            /* last pixel of the tile blue */
    const lj_cell *r2 = lj_vt_row(vt, 2);
    assert((r2[2].fg & LJ_VT_FG_PIXELS) && r2[2].marks[1] && r2[2].marks[1] != id);
    assert(tiles[r2[2].marks[1]][3*CW + 2] == 0x0000ff); /* rows 20..23 of the image are blue */
    assert(tiles[r2[2].marks[1]][5*CW + 2] == (0xbb0000 | 4)); /* below the image: tile bg untouched */
    assert(lj_vt_state(vt, LJ_VT_CY) == 3 && lj_vt_state(vt, LJ_VT_CX) == 0); /* cursor below the image */
    /* a transparent sixel over a FILLED cell keeps the fill as the tile ground
     * (the fill was painted with SGR 44 and the SGR reset before the image) */
    feed(vt, "\033[5;3H\033[44m   \033[0m\033[5;3H\033P0;1;0q#0;2;100;0;0#0!3~\033\\");
    assert(last_bg == 4);                                /* cell bg (index 4), not the reset SGR bg */
    assert(lj_vt_row(vt, 4)[2].marks[1] && (lj_vt_row(vt, 4)[2].fg & LJ_VT_FG_PIXELS));
    /* A NEW image over a cell that already has a tile starts from the ground:
     * a marching overlay re-emitted at the same place must not accumulate
     * its previous positions (the operator 2026-09-12: "imprints accumulate"). */
    { feed(vt, "\033[7;7H\033P0;1;0q#0;2;100;0;0#0~\033\\");            /* dot at px 0 */
      uint32_t tid = lj_vt_row(vt, 6)[6].marks[1]; assert(tid);
      assert((tiles[tid][0] & 0xffffff) == 0xff0000);
      feed(vt, "\033[7;7H\033P0;1;0q#0;2;100;0;0#0???~\033\\");         /* same cell, dot at px 3 */
      assert(lj_vt_row(vt, 6)[6].marks[1] == tid);                           /* tile reused (no id churn) */
      assert((tiles[tid][3] & 0xffffff) == 0xff0000);
      assert((tiles[tid][0] & 0xffffff) != 0xff0000);                        /* old dot gone */ }
    /* the VT reports exactly the tile ids still referenced (visible + scrollback),
     * so the app never recycles a live id (the "mirroring" defect) */
    { nseen = 0;
      feed(vt, "\033[7;7H\033P0;1;0q#0;2;100;0;0#0~\033\\");            /* a tile at (7,7) */
      uint32_t live = lj_vt_row(vt, 6)[6].marks[1]; assert(live);
      lj_vt_tiles(vt, walk, NULL);
      int found = 0; for (int i = 0; i < nseen; i++) if (seen[i] == live) found++;
      assert(found == 1);                                                    /* our cell's tile is live */
      feed(vt, "\033[7;7HX");                                                /* overwrite it */
      nseen = 0; lj_vt_tiles(vt, walk, NULL); found = 0; for (int i = 0; i < nseen; i++) if (seen[i] == live) found++;
      assert(found == 0); }                                                  /* gone: safe to recycle */
    /* icon-width-contract: 🛠 U+1F6E0 (wcwidth 1) + U+FE0F occupies two cells, cursor advances 2, VS16 not stored */
    feed(vt, "\033[8;1H\xF0\x9F\x9B\xA0\xEF\xB8\x8FX");
    assert(lj_vt_row(vt, 7)[0].ch == 0x1F6E0 && lj_vt_row(vt, 7)[1].ch == 0 && lj_vt_row(vt, 7)[2].ch == 'X');
    assert(lj_vt_row(vt, 7)[0].marks[0] != 0xFE0F);
    /* image_end fires exactly once per image, at ST, even when the image arrives in two feeds */
    { int before = image_ends;
      feed(vt, "\033[9;9H\033P0;1;0q#0;2;100;0;0#0~~");   /* first half: no ST yet */
      assert(image_ends == before);
      feed(vt, "~~\033\\");                              /* second half + ST */
      assert(image_ends == before + 1); }
    /* text overwrites the cell: the pixel flag and tile id go away */
    feed(vt, "\033[2;3HX");
    assert(lj_vt_row(vt, 1)[2].ch == 'X' && !(lj_vt_row(vt, 1)[2].fg & LJ_VT_FG_PIXELS) && !lj_vt_row(vt, 1)[2].marks[1]);
    /* the tile id survives a scroll */
    uint32_t keep = lj_vt_row(vt, 2)[2].marks[1];
    feed(vt, "\033[10;1H\n");
    assert(lj_vt_row(vt, 1)[2].marks[1] == keep);
    /* an image on the bottom row NEVER scrolls (WezTerm behaviour, measured): the
     * cursor stays on the last row and the image lands where it was placed */
    feed(vt, "\033[9;1Hstay\033[10;1H\033Pq#0!5~\033\\");
    assert(lj_vt_state(vt, LJ_VT_CY) == 9);
    assert(lj_vt_row(vt, 9)[0].fg & LJ_VT_FG_PIXELS);       /* row 10 holds the image */
    assert(lj_vt_row(vt, 8)[0].ch == 's');                    /* row 9 did not move */
    /* DECSDM on: an image on the last row neither moves the cursor nor scrolls (lilJack's ring on the bottom row) */
    feed(vt, "\033[?80h\033[9;1Hkeep\033[10;5H\033Pq#0!5~\033\\");
    assert(lj_vt_state(vt, LJ_VT_CY) == 9 && lj_vt_state(vt, LJ_VT_CX) == 4);   /* cursor untouched */
    assert(lj_vt_row(vt, 8)[0].ch == 'k');                                       /* no scroll: row 9 still holds "keep" */
    assert(lj_vt_row(vt, 9)[4].fg & LJ_VT_FG_PIXELS);                            /* image landed on row 10 */
    feed(vt, "\033[?80l");
    /* switching the sink off restores half blocks */
    lj_vt_set_pixel_sink(vt, NULL);
    feed(vt, "\033[c"); assert(!strcmp(lj_vt_reply(vt), "\033[?1;2c")); lj_vt_clear_reply(vt);
    feed(vt, "\033[16t"); assert(!*lj_vt_reply(vt));           /* no pixel knowledge without a sink: silent */
    feed(vt, "\033[18t"); assert(!strcmp(lj_vt_reply(vt), "\033[8;10;40t")); lj_vt_clear_reply(vt);
    lj_vt_free(vt);
    puts("vt-pixels: ok");
    return 0;
}
