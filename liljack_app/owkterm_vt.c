/* Desktop adapter derived from the handheld/app/owkterm.c.
 * Handheld parser and scroll-region behavior retained; instance state,
 * Unicode, mouse/alternate screen, resizing and color added for lilJack.
 * Source SHA256: 1127b4c30b4c0fe3058e7d43731f78250be5f183d996529731a580074abf44a5
 * Not a change to the deployed handheld terminal. Single UI thread only.
 */
#define _XOPEN_SOURCE 700
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>
#include <locale.h>
#include "owkterm_vt.h"
#define MAXCOLS LJ_VT_MAXCOLS
#define MAXROWS LJ_VT_MAXROWS
#define SB_LINES 512
#define DEF_FG 7
#define DEF_BG 0
typedef lj_cell cell;
enum { ST_GROUND, ST_ESC, ST_CSI, ST_SKIP1, ST_OSC, ST_OSC_ESC, ST_OSC_P,
       ST_DCS, ST_DCS_ESC };
typedef struct {
 cell grid[MAXROWS][MAXCOLS], saved[MAXROWS][MAXCOLS], sb[SB_LINES][MAXCOLS];
 unsigned char dirty[MAXROWS];
 int cols,rows,cx,cy,bold,rev,acs,top,bot,saved_x,saved_y,cursor_on;
 uint32_t fg,bg;
 int sb_head,sb_count,view,sb_enable,dec,st,par[32],npar,priv,osc_n;
 int alt,normal_x,normal_y,mouse,sgr_mouse,pixel_mouse,paste,app_cursor;
 int sync;                             /* DECSET 2026: presenter holds the frame until DECRST */
 unsigned long long scrolled;          /* rows that left the top of the normal screen, monotonic (selection anchor) */
 int remaining; uint32_t unicode,min_unicode;
 /* ── sixel (DCS ... q) ────────────────────────────────────────────────
  * ⚠ ESC P WAS NOT HANDLED AT ALL. It fell through ST_ESC to ST_GROUND, so
  * every byte of an image payload was printed as TEXT — an agent that drew a
  * picture sprayed thousands of characters across its tile. Dropping it would
  * have been better; drawing it is better still.
  * The decode writes straight into CELLS as half blocks (▀, fg = upper pixel,
  * bg = lower), so an image is ordinary cell content: bounded by the grid,
  * scrolled and cleared by the same code as text, and incapable of leaking
  * memory or outliving its tile. */
 int sixel, sx, sy, srep, scolor, snum, sparam, sparams[8], snparam;
 uint32_t spal[256];
 lj_vt_pixel_sink sink; int has_sink;   /* pixel-level sixel, see owkterm_vt.h */
 int sdm;                               /* DECSDM (CSI ? 80 h): sixel display mode, cursor untouched */
 uint32_t sixel_gen;                    /* bumped per DCS q: a cell's tile is reset once per image */
 char reply[64];
 /* OSC 52 (clipboard set). lilJack copies a selection with ESC ] 52 ; c ; base64 ST;
  * WezTerm honours it, we did not — the operator: "I can't copy from lilJack/lilJack app".
  * The payload is buffered here (bounded), decoded at the terminator and handed
  * to the app once via lj_vt_take_clipboard. A '?' payload (read the clipboard
  * back to the child) is ignored on purpose. */
 char osc[98304]; int osc_len, osc_over;
 char *clip;
 /* APP MESSAGES (owkTerm, 2026-09-15): OSC 0/2 (window title), OSC 7 (cwd as a
  * file: URL) and OSC 7771 (owkTerm's own app channel: right-button menu items
  * and the like) are queued verbatim, bounded, and handed to the app one at a
  * time via lj_vt_take_app_osc. The VT stays a parser; policy lives in the app. */
 char *app[32]; int napp;
} OwkVT;
static int b64v(int c){if(c>='A'&&c<='Z')return c-'A';if(c>='a'&&c<='z')return c-'a'+26;if(c>='0'&&c<='9')return c-'0'+52;if(c=='+')return 62;if(c=='/')return 63;return -1;}
static void osc_dispatch(void);
static OwkVT *active;
#define COLS (active->cols)
#define ROWS (active->rows)
#define g_grid (active->grid)
#define g_dirty (active->dirty)
#define g_cx (active->cx)
#define g_cy (active->cy)
#define g_fg (active->fg)
#define g_bg (active->bg)
#define g_bold (active->bold)
#define g_rev (active->rev)
#define g_acs (active->acs)
#define g_top (active->top)
#define g_bot (active->bot)
#define g_saved_x (active->saved_x)
#define g_saved_y (active->saved_y)
#define g_cursor_on (active->cursor_on)
#define g_sb (active->sb)
#define g_sb_head (active->sb_head)
#define g_sb_count (active->sb_count)
#define g_view (active->view)
#define g_sb_enable (active->sb_enable)
#define g_dec (active->dec)
#define g_st (active->st)
#define g_par (active->par)
#define g_npar (active->npar)
#define g_priv (active->priv)
#define g_osc_n (active->osc_n)
static uint32_t palette256(int n) {
 if(n<16) return n<0?0:(uint32_t)n;
 if(n>255)n=255;
 if(n>=232) { int c=8+(n-232)*10; return 0x1000000 | c<<16 | c<<8 | c; }
 n-=16; int r=n/36,g=(n/6)%6,b=n%6;
 return 0x1000000 | (r?55+r*40:0)<<16 | (g?55+g*40:0)<<8 | (b?55+b*40:0);
}
static void set_mode(int mode,int on) {
 if(mode==25)g_cursor_on=on;
 if(mode==1)active->app_cursor=on;
 if(mode==1000 || mode==1002 || mode==1003)active->mouse=on?mode:0;
 if(mode==1006)active->sgr_mouse=on;
 if(mode==1016){active->pixel_mouse=on;if(on)active->sgr_mouse=1;}   /* xterm: 1016 implies SGR encoding */
 if(mode==2004)active->paste=on;
 if(mode==2026)active->sync=on;         /* synchronized output: lilJack fences each frame (text + graph images) */
 if(mode==80)active->sdm=on;            /* DECSDM: an image never moves the cursor or scrolls */
 if(mode==47 || mode==1047 || mode==1049) {
  if(on && !active->alt) {
   memcpy(active->saved,g_grid,sizeof(active->saved));
   active->normal_x=g_cx;active->normal_y=g_cy;
   memset(g_grid,0,sizeof(active->grid));
   for(int y=0;y<ROWS;y++)for(int x=0;x<COLS;x++) g_grid[y][x]=(cell){.ch=' ',.fg=DEF_FG,.bg=DEF_BG};
   g_cx=g_cy=0;active->alt=1;g_sb_enable=0;
  } else if(!on && active->alt) {
   memcpy(g_grid,active->saved,sizeof(active->grid));
   g_cx=active->normal_x;g_cy=active->normal_y;active->alt=0;g_sb_enable=1;
  }
  g_top=0;g_bot=ROWS-1;g_view=0;
 }
}
static void row_dirty(int r) { if (r >= 0 && r < ROWS) g_dirty[r] = 1; }

/* ---- scrollback ring ---------------------------------------------------
 * Lines that scroll off the top of a full-screen region are copied here.
 * Shift+Up/Down (line) and Shift+PgUp/PgDn (half page) browse it; any
 * other key snaps back live and is forwarded as usual. Rendering while
 * browsing is a frozen full repaint from view_row(); the live grid keeps
 * consuming pty output underneath and repaints on exit via dirty flags. */


static int sb_accepting(void) { return g_sb_enable && g_top == 0 && g_bot == ROWS - 1; }

/* Push a full-width line into the scrollback ring. The tail beyond COLS is
 * blanked so a later width-grow never exposes stale cells. */
static void sb_push(const cell *row) {
    memcpy(g_sb[g_sb_head], row, sizeof(cell) * COLS);
    for (int c = COLS; c < MAXCOLS; c++) {
        cell *cl = &g_sb[g_sb_head][c];
        cl->ch = ' '; cl->fg = DEF_FG; cl->bg = DEF_BG;
        cl->marks[0] = cl->marks[1] = cl->marks[2] = 0;
    }
    g_sb_head = (g_sb_head + 1) % SB_LINES;
    if (g_sb_count < SB_LINES) g_sb_count++;
}

static void sb_capture(void) {               /* row g_top is about to die */
    if (sb_accepting()) sb_push(g_grid[0]);
}

/* screen row -> cell row in the browsed view (ring tail + grid top) */
static const cell *view_row(int r) {
    int vi = g_sb_count - g_view + r;        /* virtual line index */
    if (vi >= g_sb_count) return g_grid[vi - g_sb_count];
    return g_sb[(g_sb_head - g_sb_count + vi + 2 * SB_LINES) % SB_LINES];
}

static void sb_scroll(int d) {
    /* ⚠ THE ALTERNATE SCREEN HAS NO SCROLLBACK. opencode/Claude full-screen TUIs
     * enter mode 47/1049, which sets sb_enable=0 — nothing is ever pushed, and
     * g_sb_count is whatever it was before alt. Browsing here would mix stale
     * ring lines into the live alt grid: the operator's "lines all over the place"
     * after scrolling the lead tile. A wheel over an alt-screen app is not a
     * scrollback browse; it is nothing (the app owns its own scrolling). */
    if (!g_sb_enable) return;
    g_view += d;
    if (g_view > g_sb_count) g_view = g_sb_count;
    if (g_view < 0) g_view = 0;
}

static void clear_cells(int r, int c0, int c1) {
    if (r < 0 || r >= ROWS) return;
    if (c0 < 0) c0 = 0;
    if (c0 > c1 || c0 >= COLS) return;
    if (c1 >= COLS) c1 = COLS-1;
    if (c0 > 0 && !g_grid[r][c0].ch && wcwidth(g_grid[r][c0-1].ch)==2) c0--;
    if (c1+1 < COLS && !g_grid[r][c1+1].ch && wcwidth(g_grid[r][c1].ch)==2) c1++;
    for (int c = c0; c <= c1 && c < COLS; c++) {
        memset(&g_grid[r][c], 0, sizeof(cell)); g_grid[r][c].ch = ' ';
        g_grid[r][c].fg = g_fg;
        g_grid[r][c].bg = g_bg;
    }
    row_dirty(r);
}

static void scroll_up(int n) {
    if (n <= 0) return;
    int region = g_bot - g_top + 1;
    if (n > region) n = region;                 /* cannot scroll more than the region */
    if (!active->alt && g_top == 0) active->scrolled += (unsigned long long)n;   /* whole-screen scroll: lilJack's selection anchor (LJ_VT_SCROLLED) */
    if (sb_accepting())
        for (int i = 0; i < n; i++) sb_push(g_grid[g_top + i]);
    if (region > n)
        memmove(&g_grid[g_top], &g_grid[g_top + n],
                sizeof(cell) * MAXCOLS * (region - n));
    for (int r = g_bot - n + 1; r <= g_bot; r++) clear_cells(r, 0, COLS - 1);
    for (int r = g_top; r <= g_bot; r++) row_dirty(r);
}

static void scroll_down(int n) {
    if (n <= 0) return;
    int region = g_bot - g_top + 1;
    if (n > region) n = region;                 /* cannot scroll more than the region */
    if (region > n)
        memmove(&g_grid[g_top + n], &g_grid[g_top],
                sizeof(cell) * MAXCOLS * (region - n));
    for (int r = g_top; r < g_top + n; r++) clear_cells(r, 0, COLS - 1);
    for (int r = g_top; r <= g_bot; r++) row_dirty(r);
}

/* Box-drawing renders PROCEDURALLY in draw_cell (continuous 1px lines
 * spanning the whole cell - font glyphs would leave gaps between cells,
 * turning mc's frames into dashes). Cells store canonical CP437 codes;
 * both charset paths normalize into them. */
static unsigned char acs_map(unsigned char c) {
    switch (c) {
    case 0xB3: case 0xBA: return 0xB3;                /* vertical */
    case 0xC4: case 0xCD: return 0xC4;                /* horizontal */
    case 0xDA: case 0xC9: return 0xDA;                /* corners */
    case 0xBF: case 0xBB: return 0xBF;
    case 0xC0: case 0xC8: return 0xC0;
    case 0xD9: case 0xBC: return 0xD9;
    case 0xC5: case 0xCE: return 0xC5;                /* cross */
    case 0xC3: case 0xCC: return 0xC3;                /* tees */
    case 0xB4: case 0xB9: return 0xB4;
    case 0xC2: case 0xCB: return 0xC2;
    case 0xC1: case 0xCA: return 0xC1;
    case 0xB0: case 0xB1: case 0xB2: case 0xDB: return c;  /* shades */
    case 0x04: return 0x04;  /* diamond */
    case 0x18: return '^';
    case 0x19: return 'v';
    case 0x1A: return '>';
    case 0x1B: return '<';
    default: return (c >= 32 && c < 127) ? c : '?';
    }
}

/* DEC special graphics (ESC)0 + SO): ncurses on TERM=linux draws mc's
 * frames with these letter codes, NOT CP437 - unmapped they render as
 * literal 'lqqqk' gibberish (captured from mc on the device) */
static unsigned char dec_acs_map(unsigned char c) {
    switch (c) {
    case 'q': return 0xC4;                            /* horizontal */
    case 'x': return 0xB3;                            /* vertical */
    case 'l': return 0xDA;                            /* corners */
    case 'k': return 0xBF;
    case 'm': return 0xC0;
    case 'j': return 0xD9;
    case 'n': return 0xC5;                            /* cross */
    case 't': return 0xC3;                            /* tees */
    case 'u': return 0xB4;
    case 'w': return 0xC2;
    case 'v': return 0xC1;
    case 'a': return 0xB1;                            /* checkerboard */
    case '0': return 0xDB;                            /* solid block */
    case 'h': return 0xB0;                            /* board of squares */
    case '`': return 0x04;                            /* diamond */
    case '~': return '.';                             /* bullet */
    case 'o': case 's': return 0xC4;                  /* scan lines */
    default: return (c >= 32 && c < 127) ? c : '?';
    }
}


static uint32_t box_unicode(uint32_t c) {
    switch(c) {
    case 0xB3:return 0x2502; case 0xC4:return 0x2500; case 0xDA:return 0x250C;
    case 0xBF:return 0x2510; case 0xC0:return 0x2514; case 0xD9:return 0x2518;
    case 0xC5:return 0x253C; case 0xC3:return 0x251C; case 0xB4:return 0x2524;
    case 0xC2:return 0x252C; case 0xC1:return 0x2534; case 0xB0:return 0x2591;
    case 0xB1:return 0x2592; case 0xB2:return 0x2593; case 0xDB:return 0x2588;
    default:return c;
    }
}
/* ── sixel ───────────────────────────────────────────────────────────────
 * One byte at a time, so a half-received image costs nothing and a truncated
 * one simply stops. Everything is clipped to the grid: a hostile width, a
 * repeat count of a million, or a colour index past the palette can move the
 * pen nowhere the cursor could not already go.
 */
#define SIXEL_MAX_REPEAT 4096          /* a repeat is a run length, not a loop */

static void sixel_plot(int px, int py, uint32_t rgb) {
    OwkVT *v = active;
    if (v->has_sink) {
        /* pixel mode: real pixels go to the app's tile for this cell; the
         * half-block colours below stay an honest low-res fallback */
        int cw = v->sink.cell_w, ch = v->sink.cell_h;
        int col = g_cx + px / cw, row = g_cy + py / ch;
        if (col < 0 || col >= COLS || row < 0 || row >= ROWS) return;
        cell *k = &g_grid[row][col];
        if (k->ch == 0x2580 && (k->fg & LJ_VT_FG_PIXELS) && k->marks[1] && k->marks[2] != v->sixel_gen) {
            /* first pixel of THIS image into a cell that already holds a tile:
             * start from the ground again, do not stack on the previous image */
            /* marks[0] holds the cell's ORIGINAL ground (k->bg is rewritten by
             * the half-block fallback with the last plotted pixel colour, so it
             * cannot be the ground: it painted whole tiles in the dot colour). */
            uint32_t ground = k->marks[0];
            if (v->sink.tile_reset) v->sink.tile_reset(v->sink.user, k->marks[1], ground);
            else k->marks[1] = v->sink.tile_new(v->sink.user, ground);
            k->marks[2] = v->sixel_gen;
        }
        if (k->ch != 0x2580 || !(k->fg & LJ_VT_FG_PIXELS) || !k->marks[1]) {
            /* The tile's ground is the cell's EXISTING background when it has
             * one, else the current SGR background: a transparent (P2=1) sixel
             * over a filled cell must leave the fill visible, as WezTerm and
             * xterm do. Using g_bg here erased the fill of every cell a
             * transparent overlay (lilJack's lead ring) touched. */
            uint32_t raw = k->bg ? k->bg : g_bg;
            uint32_t bg = 0x1000000u | (raw & 0xffffff);
            if (!(raw & 0x1000000u)) bg = raw;                    /* indexed: keep the index */
            k->ch = 0x2580; k->fg = LJ_VT_FG_PIXELS | bg; k->bg = bg;
            k->marks[0] = raw; k->marks[2] = v->sixel_gen;         /* marks[0] = ground, marks[2] = image generation */
            k->marks[1] = v->sink.tile_new(v->sink.user, raw);    /* raw: index or truecolor, the app resolves */
        }
        v->sink.tile_plot(v->sink.user, k->marks[1], px % cw, py % ch, rgb & 0xffffff);
        if (py % ch < ch / 2) k->fg = LJ_VT_FG_PIXELS | 0x1000000u | (rgb & 0xffffff);
        else                  k->bg = 0x1000000u | (rgb & 0xffffff);
        g_dirty[row] = 1;
        return;
    }
    /* Two pixel rows share one cell: upper -> fg of ▀, lower -> bg. */
    int col = g_cx + px, row = g_cy + py / 2;
    if (col < 0 || col >= COLS || row < 0 || row >= ROWS) return;
    cell *k = &g_grid[row][col];
    if (k->ch != 0x2580) {             /* first touch: make it a half block */
        k->ch = 0x2580; k->fg = 0x1000000u | (g_bg & 0xffffff);
        k->bg = 0x1000000u | (g_bg & 0xffffff);
        k->marks[0] = k->marks[1] = k->marks[2] = 0;
    }
    if (py % 2 == 0) k->fg = 0x1000000u | (rgb & 0xffffff);
    else             k->bg = 0x1000000u | (rgb & 0xffffff);
    g_dirty[row] = 1;
}

/* Finish a pending "#…" run. ⚠ THE FIRST PARAMETER IS THE INDEX, NOT THE
 * FORMAT: "#0;2;100;0;0" is index 0, format 2 (RGB), then r,g,b. Reading slot 0
 * as the format made every definition fail its own test, so every colour after
 * the first silently kept the default and a two-colour image drew in one. */
static void sixel_colour(OwkVT *v) {
    if (v->snum >= 0 && v->snparam < 8) v->sparams[v->snparam++] = v->snum;
    v->snum = -1;
    if (!v->snparam) return;
    int idx = v->sparams[0] & 255;
    if (v->snparam >= 5 && v->sparams[1] == 2) {          /* define, RGB 0..100 */
        int r = v->sparams[2], g = v->sparams[3], b = v->sparams[4];
        r = r < 0 ? 0 : r > 100 ? 100 : r;
        g = g < 0 ? 0 : g > 100 ? 100 : g;
        b = b < 0 ? 0 : b > 100 ? 100 : b;
        v->spal[idx] = ((r*255/100) << 16) | ((g*255/100) << 8) | (b*255/100);
    }
    v->scolor = idx;                                      /* define also selects */
    v->snparam = 0;
}

static void sixel_byte(unsigned char c) {
    OwkVT *v = active;
    if (!v->sixel) {                   /* still in the DCS parameter prologue */
        if (c >= '0' && c <= '9') { if (v->sparam < 0) v->sparam = 0;
                                     if (v->sparam < 100000) v->sparam = v->sparam*10 + (c-'0'); return; }
        if (c == ';') { if (v->snparam < 8) v->sparams[v->snparam++] = v->sparam < 0 ? 0 : v->sparam;
                        v->sparam = -1; return; }
        if (c == 'q') { v->sixel = 1; v->sparam = -1; v->snum = -1; v->sixel_gen++; if (!v->sixel_gen) v->sixel_gen = 1; return; }
        /* ⚠ Any other final byte is a DCS we do not implement (DECRQSS, a
         * terminfo query). It is consumed to the terminator and never drawn —
         * that silence is the point, since printing it is what corrupted the
         * screen before. */
        v->sixel = -1; return;
    }
    if (v->sixel < 0) return;          /* a DCS that is not sixel: swallow it */

    if (c >= '0' && c <= '9') { if (v->snum < 0) v->snum = 0;
                                if (v->snum < 1000000) v->snum = v->snum*10 + (c-'0'); return; }
    switch (c) {
    case '#':                          /* colour: "#n" selects, "#n;2;r;g;b" defines */
        sixel_colour(v);               /* finish the previous one first */
        v->snparam = 0; v->snum = -1; v->srep = 1;
        return;
    case ';':
        if (v->snparam < 8) v->sparams[v->snparam++] = v->snum < 0 ? 0 : v->snum;
        v->snum = -1;
        return;
    case '!':
        /* ⚠ THE COUNT FOLLOWS THE '!', IT DOES NOT PRECEDE IT. This read
         * v->snum — the digits seen BEFORE the '!' — of which there are none,
         * so every run drew ONE column instead of n. Worse, the count then
         * survived into the colour path: at the data byte sixel_colour() took
         * the '4' of "!4~" as a colour INDEX and selected undefined register 4,
         * so the run also lost its colour. One defect, two symptoms.
         *
         * Invisible until the seam was tested, because hand-written test sixels
         * use no repeats: measured on tmux's own re-encoding of a 4x12 two-
         * colour image, "#0!4~-#1!4~--" drew 6 half-blocks in 1 colour where it
         * owes 24 in 2. srep == 0 now means "armed, the count is coming". */
        /* ⚠ AND A PENDING COLOUR SELECTION MUST BE FLUSHED FIRST. In "#0!4~"
         * the 0 of "#0" is still sitting in snum when '!' arrives; clearing
         * snum here threw the SELECTION away, so the run drew in whatever
         * colour was selected last. tmux emits exactly that shape, so a
         * two-colour image rendered entirely in one colour — 24 half-blocks
         * in 1 colour where it owes 2. Flush, then arm. */
        sixel_colour(v);
        v->srep = 0;
        v->snum = -1;
        return;
    case '$': sixel_colour(v); v->sx = 0; v->snum = -1; v->srep = 1; return; /* CR */
    case '-': sixel_colour(v); v->sx = 0; v->sy += 6; v->snum = -1; v->srep = 1; return;
    default: break;
    }
    if (c < '?' || c > '~') { v->snum = -1; return; }        /* not a sixel byte */

    {
        /* ⚠ CONSUME THE REPEAT COUNT BEFORE sixel_colour() CAN SEE IT. While
         * '!' is armed the pending digits are a run length, not a colour
         * index; leaving them for sixel_colour() is what silently reselected
         * an undefined palette entry. */
        int reps = 1;
        if (v->srep == 0) {
            reps = v->snum < 1 ? 1 : (v->snum > SIXEL_MAX_REPEAT ? SIXEL_MAX_REPEAT : v->snum);
            v->snum = -1;
        } else if (v->srep > 1) reps = v->srep;
        sixel_colour(v);               /* a pending #… completes at the first data byte */
        int bits = c - '?';
        uint32_t rgb = v->spal[v->scolor & 255];
        for (int n = 0; n < reps; n++, v->sx++) {
            if (g_cx + v->sx / (v->has_sink ? v->sink.cell_w : 1) >= COLS) break; /* clipped, never wrapped */
            for (int b = 0; b < 6; b++)
                if (bits & (1 << b)) sixel_plot(v->sx, v->sy + b, rgb);
        }
        v->srep = 1; v->snum = -1;
    }
}

/* End of a sixel string. In pixel mode the cursor moves to the row below the
 * image (clamped to the last row, never scrolling — WezTerm's behaviour);
 * half-block mode keeps the cursor where it was, which is what lilJack's
 * presenter relies on. */
static void sixel_end(void) {
    OwkVT *v = active;
    if (v->sixel != 1 || !v->has_sink) return;
    /* DECSDM (xterm >= 369 semantics, WezTerm, foot, mlterm): the image is
     * placed and the cursor stays exactly where it was. lilJack's presenter
     * paints its lead ring on the LAST screen row inside DECSC/DECRC; without
     * this the row below the ring does not exist, the VT scrolled the whole
     * screen up, and the diff-based presenter then repainted everything —
     * the operator's "opens the owkTerm tab, clears the screen, scrolls up buggy". */
    if (v->sink.image_end) v->sink.image_end(v->sink.user);
    if (v->sdm) { v->sixel = 0; return; }
    /* ⚠ NO SCROLL AFTER AN IMAGE (measured against WezTerm, 2026-09-12). The
     * cursor moves to the row below the image when that row exists and stays
     * on the last row otherwise. xterm scrolls here; WezTerm does not, and
     * lilJack paints its lead ring on the LAST row inside DECSC/DECRC — with
     * the scroll the whole screen shifted under lilJack's diff pass and it
     * repainted everything (the operator: "opens the tab, clears the screen, scrolls
     * up buggy"). An app that wants xterm's scroll can print a newline. */
    int rows = (v->sy + 6 + v->sink.cell_h - 1) / v->sink.cell_h;
    for (int i = 0; i < rows; i++) if (g_cy < ROWS - 1) g_cy++;
    g_cx = 0;
    v->sixel = 0;
}

static void put_char(uint32_t c) {
    if (g_dec) c = box_unicode(dec_acs_map((unsigned char)c));
    else if (g_acs) c = box_unicode(acs_map((unsigned char)c));
    int width = wcwidth((wchar_t)c);
    if (width < 0) width = 1;
    /* ⚠ U+FE0F EMOJI PRESENTATION WIDENS. A width-1 pictograph followed by VS16
     * (🛠︎ → 🛠️) occupies TWO cells in WezTerm/foot/kitty; lilJack's presenter
     * emits VS16 for exactly those icons (icon-width-contract, 2026-09-13). The
     * selector is consumed here, never stored as a mark (no face has a glyph
     * for it, so a stored mark drew a tofu box). */
    if (c == 0xFE0F) {
        int x = g_cx > 0 ? g_cx - 1 : 0;
        cell *b = &g_grid[g_cy][x];
        if (b->ch && g_cx < COLS && wcwidth((wchar_t)b->ch) == 1 && g_grid[g_cy][g_cx].ch != 0) {
            /* widen: the cell at the cursor becomes the continuation */
            if (g_cx + 1 < COLS && !g_grid[g_cy][g_cx + 1].ch) clear_cells(g_cy, g_cx + 1, g_cx + 1);
            g_grid[g_cy][g_cx] = *b; g_grid[g_cy][g_cx].ch = 0; row_dirty(g_cy); g_cx++;
        } else if (b->ch && g_cx < COLS && wcwidth((wchar_t)b->ch) == 1 && g_grid[g_cy][g_cx].ch == 0 && x == g_cx - 1) {
            /* the cell at the cursor is already a continuation of b (re-widen): nothing to do */
        }
        return;
    }
    if (width == 0) {
        int x = g_cx > 0 ? g_cx - 1 : 0;
        if (g_grid[g_cy][x].ch == 0 && x > 0) x--;
        for (int i=0;i<3;i++) if (!g_grid[g_cy][x].marks[i]) {
            g_grid[g_cy][x].marks[i]=c; break;
        }
        return;
    }
    if (g_cx + width > COLS) {
        g_cx = 0;
        if (g_cy == g_bot) scroll_up(1); else if (g_cy < ROWS - 1) g_cy++;
    }
    if (width > COLS) width = 1;
    cell *cl = &g_grid[g_cy][g_cx];
    if (!cl->ch && g_cx > 0) clear_cells(g_cy,g_cx-1,g_cx-1);
    if (g_cx+1<COLS && !g_grid[g_cy][g_cx+1].ch) clear_cells(g_cy,g_cx+1,g_cx+1);
    memset(cl,0,sizeof(*cl)); cl->ch=c;
    cl->fg=g_rev ? g_bg : (g_bold && g_fg < 8 ? g_fg+8 : g_fg);
    cl->bg=g_rev ? g_fg : g_bg;
    if (width == 2) { g_grid[g_cy][g_cx+1]=*cl; g_grid[g_cy][g_cx+1].ch=0; }
    row_dirty(g_cy); g_cx += width;
}

/* ---- VT parser --------------------------------------------------------- */



static void csi_dispatch(unsigned char f) {
    int p0 = g_npar > 0 ? g_par[0] : 0;
    int p1 = g_npar > 1 ? g_par[1] : 0;
    if(g_cx>=COLS) g_cx=COLS-1;
    if(g_cy>=ROWS) g_cy=ROWS-1;

    switch (f) {
    case 'n': if(p0==6) snprintf(active->reply,sizeof(active->reply),"\033[%d;%dR",g_cy+1,(g_cx<COLS?g_cx:COLS-1)+1); else if(p0==5) strcpy(active->reply,"\033[0n"); break;
    case 'c': strcpy(active->reply, active->has_sink ? "\033[?62;4;22c" : "\033[?1;2c"); break;
    /* XTWINOPS reports (xterm): 14 = text area in pixels, 16 = cell in pixels,
     * 18 = text area in cells. Pixel replies need a real cell size, which only
     * a pixel sink (owkTerm) knows; without one they are silently ignored like
     * a terminal without pixel knowledge. lilJack's cell probe (c_ansi.c) asks
     * 16 t first, so this is what makes its grid match the window. */
    case 't': {
        int cw = active->has_sink ? active->sink.cell_w : 0, ch = active->has_sink ? active->sink.cell_h : 0;
        if (p0 == 14 && cw > 0) snprintf(active->reply, sizeof(active->reply), "\033[4;%d;%dt", ROWS * ch, COLS * cw);
        else if (p0 == 16 && cw > 0) snprintf(active->reply, sizeof(active->reply), "\033[6;%d;%dt", ch, cw);
        else if (p0 == 18) snprintf(active->reply, sizeof(active->reply), "\033[8;%d;%dt", ROWS, COLS);
        break; }
    case 'A': g_cy -= p0 ? p0 : 1; if (g_cy < g_top) g_cy = g_top; break;
    case 'B': g_cy += p0 ? p0 : 1; if (g_cy > g_bot) g_cy = g_bot; break;
    case 'C': g_cx += p0 ? p0 : 1; if (g_cx >= COLS) g_cx = COLS - 1; break;
    case 'D': g_cx -= p0 ? p0 : 1; if (g_cx < 0) g_cx = 0; break;
    case 'E': g_cx = 0; g_cy += p0 ? p0 : 1; if (g_cy > g_bot) g_cy = g_bot; break;
    case 'F': g_cx = 0; g_cy -= p0 ? p0 : 1; if (g_cy < g_top) g_cy = g_top; break;
    case 'G': g_cx = (p0 ? p0 : 1) - 1;
              if (g_cx < 0) g_cx = 0; if (g_cx >= COLS) g_cx = COLS - 1; break;
    case 'd': g_cy = (p0 ? p0 : 1) - 1;
              if (g_cy < 0) g_cy = 0; if (g_cy >= ROWS) g_cy = ROWS - 1; break;
    case 'H': case 'f':
        g_cy = (p0 ? p0 : 1) - 1; g_cx = (p1 ? p1 : 1) - 1;
        if (g_cy < 0) g_cy = 0; if (g_cy >= ROWS) g_cy = ROWS - 1;
        if (g_cx < 0) g_cx = 0; if (g_cx >= COLS) g_cx = COLS - 1;
        break;
    case 'J':
        if (p0 == 0) { clear_cells(g_cy, g_cx, COLS - 1);
                       for (int r = g_cy + 1; r < ROWS; r++) clear_cells(r, 0, COLS - 1); }
        else if (p0 == 1) { for (int r = 0; r < g_cy; r++) clear_cells(r, 0, COLS - 1);
                            clear_cells(g_cy, 0, g_cx); }
        else { for (int r = 0; r < ROWS; r++) clear_cells(r, 0, COLS - 1); }
        break;
    case 'K':
        if (p0 == 0) clear_cells(g_cy, g_cx, COLS - 1);
        else if (p0 == 1) clear_cells(g_cy, 0, g_cx);
        else clear_cells(g_cy, 0, COLS - 1);
        break;
    case 'L': { if(g_cy<g_top || g_cy>g_bot) break; int n = p0 ? p0 : 1; if(n>ROWS)n=ROWS; int ot = g_top; g_top = g_cy;
                scroll_down(n); g_top = ot; } break;
    case 'M': { if(g_cy<g_top || g_cy>g_bot) break; int n = p0 ? p0 : 1; if(n>ROWS)n=ROWS; int ot = g_top; g_top = g_cy;
                g_sb_enable = 0;             /* edit, not output flow */
                scroll_up(n); g_sb_enable = 1; g_top = ot; } break;
    case '@': { int n = p0 ? p0 : 1; if (n > COLS - g_cx) n = COLS - g_cx;
                memmove(&g_grid[g_cy][g_cx + n], &g_grid[g_cy][g_cx],
                        sizeof(cell) * (COLS - g_cx - n));
                clear_cells(g_cy, g_cx, g_cx + n - 1); } break;
    case 'P': { int n = p0 ? p0 : 1; if (n > COLS - g_cx) n = COLS - g_cx;
                memmove(&g_grid[g_cy][g_cx], &g_grid[g_cy][g_cx + n],
                        sizeof(cell) * (COLS - g_cx - n));
                clear_cells(g_cy, COLS - n, COLS - 1); } break;
    case 'X': { int n = p0 ? p0 : 1; if (n > COLS - g_cx) n = COLS - g_cx;
                clear_cells(g_cy, g_cx, g_cx + n - 1); } break;  /* ECH:
                ncurses blanks shrinking panes with this (mc preview) */
    case 'S': scroll_up(p0 ? p0 : 1); break;
    case 'T': scroll_down(p0 ? p0 : 1); break;
    case 'r':
        g_top = (p0 ? p0 : 1) - 1;
        g_bot = (p1 ? p1 : ROWS) - 1;
        if (g_top < 0) g_top = 0;
        if (g_bot >= ROWS) g_bot = ROWS - 1;
        if (g_top >= g_bot) { g_top = 0; g_bot = ROWS - 1; }
        g_cx = 0; g_cy = g_top;
        break;
    case 'm':
        if (g_npar == 0) { g_fg = DEF_FG; g_bg = DEF_BG; g_bold = g_rev = 0; }
        for (int i = 0; i < g_npar; i++) {
            int v = g_par[i];
            if (v == 0)  { g_fg = DEF_FG; g_bg = DEF_BG; g_bold = g_rev = 0; g_acs = 0; }
            else if (v == 1)  g_bold = 1;
            else if (v == 7)  g_rev = 1;
            else if (v == 10) g_acs = 0;
            else if (v == 11 || v == 12) g_acs = 1;
            else if ((v == 38 || v == 48) && i+2 < g_npar) {
                uint32_t col=7;
                if (g_par[i+1]==5) { col=palette256(g_par[i+2]); i+=2; }
                else if(g_par[i+1]==2 && i+4<g_npar) { col=0x1000000 | ((g_par[i+2]&255)<<16) | ((g_par[i+3]&255)<<8) | (g_par[i+4]&255); i+=4; }
                if(v==38) g_fg=col; else g_bg=col;
            }
            else if (v == 22) g_bold = 0;
            else if (v == 27) g_rev = 0;
            else if (v >= 30 && v <= 37)  g_fg = v - 30;
            else if (v == 39) g_fg = DEF_FG;
            else if (v >= 40 && v <= 47)  g_bg = v - 40;
            else if (v == 49) g_bg = DEF_BG;
            else if (v >= 90 && v <= 97)  g_fg = v - 90 + 8;
            else if (v >= 100 && v <= 107) g_bg = v - 100 + 8;
        }
        break;
    case 's': g_saved_x = g_cx; g_saved_y = g_cy; break;
    case 'u': g_cx = g_saved_x; g_cy = g_saved_y; break;
    case 'h': case 'l': if (g_priv) for(int i=0;i<g_npar;i++) set_mode(g_par[i], f=='h'); break;
    default: break;                     /* ignored */
    }
}

static void vt_feed(unsigned char c) {
    switch (g_st) {
    case ST_GROUND:
        if (c == 0x1B) { g_st = ST_ESC; return; }
        if (c == '\r') { g_cx = 0; return; }
        if (c == '\n') {
            if (g_cy == g_bot) scroll_up(1);
            else if (g_cy < ROWS - 1) g_cy++;
            return;
        }
        if (c == '\b') { if (g_cx > 0) g_cx--; return; }
        if (c == '\t') { g_cx = (g_cx / 8 + 1) * 8;
                         if (g_cx >= COLS) g_cx = COLS - 1; return; }
        if (c == 0x07) return;                       /* BEL */
        if (c == 0x0E) { g_dec = 1; return; }        /* SO -> DEC graphics */
        if (c == 0x0F) { g_dec = 0; return; }        /* SI -> normal */
        if (c < 32 && !g_acs) return;
        put_char(c);
        return;
    case ST_ESC:
        if (c == 'P') {                       /* DCS: sixel lives here */
            g_st = ST_DCS; active->sixel = 0; active->snparam = 0;
            active->sparam = -1; active->srep = 1; active->scolor = 0;
            active->sx = 0; active->sy = 0; active->snum = -1;
            for (int i = 0; i < 256; i++) active->spal[i] = 0xffffff;
            return;
        }
        if (c == '[') { g_st = ST_CSI; g_npar = 0; g_priv = 0;
                        memset(g_par, 0, sizeof(g_par)); return; }
        if (c == '7') { g_saved_x = g_cx; g_saved_y = g_cy; }
        else if (c == '8') { g_cx = g_saved_x; g_cy = g_saved_y; }
        else if (c == 'M') { if (g_cy == g_top) scroll_down(1); else if (g_cy > 0) g_cy--; }
        else if (c == 'D') { if (g_cy == g_bot) scroll_up(1); else if (g_cy < ROWS - 1) g_cy++; }
        else if (c == 'E') { g_cx = 0; if (g_cy == g_bot) scroll_up(1); else if (g_cy < ROWS - 1) g_cy++; }
        else if (c == 'c') { /* full reset */
            g_fg = DEF_FG; g_bg = DEF_BG; g_bold = g_rev = g_acs = g_dec = 0;
            g_top = 0; g_bot = ROWS - 1; g_cx = g_cy = 0;
            for (int r = 0; r < ROWS; r++) clear_cells(r, 0, COLS - 1);
        }
        else if (c == '(' || c == ')' || c == '#') { g_st = ST_SKIP1; return; }
        else if (c == ']') { g_st = ST_OSC; g_osc_n = 0; return; }
        g_st = ST_GROUND;
        return;
    case ST_DCS:
        if (c == 0x1B) { g_st = ST_DCS_ESC; return; }
        if (c == 0x07) { g_st = ST_GROUND; sixel_end(); return; }
        sixel_byte(c);
        return;
    case ST_DCS_ESC:
        /* ESC \ ends the string; anything else was an ESC inside it. */
        g_st = (c == '\\') ? ST_GROUND : ST_DCS;
        if (g_st == ST_GROUND) sixel_end();
        return;
    case ST_SKIP1:
        g_st = ST_GROUND;
        return;
    case ST_OSC:
        if (g_osc_n++ == 0) {        /* linux console palette forms */
            active->osc_len = 0; active->osc_over = 0;
            if (c == 'P') { g_st = ST_OSC_P; g_osc_n = 0; return; }
            if (c == 'R') { g_st = ST_GROUND; return; }
        }
        if (c == 0x07) { g_st = ST_GROUND; osc_dispatch(); }
        else if (c == 0x1B) g_st = ST_OSC_ESC;
        else if (active->osc_len < (int)sizeof(active->osc) - 1) active->osc[active->osc_len++] = (char)c;
        else active->osc_over = 1;
        return;
    case ST_OSC_ESC:                 /* ESC \ = ST terminator */
        if (c == '\\') { g_st = ST_GROUND; osc_dispatch(); } else g_st = ST_OSC;
        return;
    case ST_OSC_P:
        if (++g_osc_n >= 7) g_st = ST_GROUND;
        return;
    case ST_CSI:
        if (c == '?') { g_priv = 1; return; }
        if (c >= '0' && c <= '9') {
            if (g_npar == 0) g_npar = 1;
            g_par[g_npar - 1] = (g_par[g_npar - 1] < 10000 ? g_par[g_npar - 1] * 10 + (c - '0') : 10000);
            return;
        }
        if (c == ';') { if (g_npar < 31) g_npar++; g_par[g_npar - 1] = 0;
                        if (g_npar == 1) g_npar = 2, g_par[0] = g_par[0], g_par[1] = 0;
                        return; }
        if (c < 0x40) return;                        /* intermediate: skip */
        csi_dispatch(c);
        g_st = ST_GROUND;
        return;
    }
}


void *lj_vt_new(int cols,int rows) {
 setlocale(LC_CTYPE, "");
 OwkVT *v=calloc(1,sizeof(*v)); if(!v)return NULL;
 active=v; COLS=cols<1?1:cols>MAXCOLS?MAXCOLS:cols;
 ROWS=rows<1?1:rows>MAXROWS?MAXROWS:rows;
 g_fg=DEF_FG;g_bg=DEF_BG;g_bot=ROWS-1;g_cursor_on=1;g_sb_enable=1;
 for(int y=0;y<ROWS;y++)clear_cells(y,0,COLS-1);
 return v;
}
void lj_vt_free(void *p){if(!p)return;OwkVT *v=p;free(v->clip);for(int i=0;i<v->napp;i++)free(v->app[i]);free(p);}
void lj_vt_tiles(void *p, void (*cb)(void *user, uint32_t id), void *user){
 OwkVT *v=p; if(!v||!cb)return;
 #define LJ_TILE_WALK(rowp) for(int c=0;c<MAXCOLS;c++){const cell *k=&(rowp)[c]; if(k->ch==0x2580&&(k->fg&LJ_VT_FG_PIXELS)&&k->marks[1])cb(user,k->marks[1]);}
 for(int y=0;y<MAXROWS;y++){LJ_TILE_WALK(v->grid[y]);LJ_TILE_WALK(v->saved[y]);}
 for(int y=0;y<SB_LINES;y++){LJ_TILE_WALK(v->sb[y]);}
 #undef LJ_TILE_WALK
}
void lj_vt_resize(void *p,int cols,int rows) {
 active=p; int oc=COLS,or=ROWS;
 COLS=cols<1?1:cols>MAXCOLS?MAXCOLS:cols;ROWS=rows<1?1:rows>MAXROWS?MAXROWS:rows;
 for(int y=0;y<ROWS;y++) {
  if(y>=or)clear_cells(y,0,COLS-1);else if(COLS>oc)clear_cells(y,oc,COLS-1);
  for(int x=0;x<COLS;x++) if(y>=or || x>=oc)
      active->saved[y][x]=(cell){.ch=' ',.fg=DEF_FG,.bg=DEF_BG};
  if(g_grid[y][COLS-1].ch && wcwidth(g_grid[y][COLS-1].ch)==2)clear_cells(y,COLS-1,COLS-1);
 }
 if(g_cx>=COLS)g_cx=COLS-1;if(g_cy>=ROWS)g_cy=ROWS-1;
 if(active->normal_x>=COLS)active->normal_x=COLS-1;
 if(active->normal_y>=ROWS)active->normal_y=ROWS-1;
 g_top=0;g_bot=ROWS-1;g_view=0;
 /* Scrollback survives a width change. sb_push stores every line at full
    MAXCOLS width with a blanked tail, so a grow exposes spaces and a shrink
    clips on render. Truncation, not reflow (documented in owkterm_vt.h). */
}
void lj_vt_feed(void *p,const unsigned char *buf,int len) {
 active=p;
 for(int i=0;i<len;i++) {
  unsigned char c=buf[i];
  if(active->remaining) {
   if((c&0xC0)==0x80) {
    active->unicode=(active->unicode<<6)|(c&63);
    if(--active->remaining==0) {
     uint32_t cp=active->unicode;
     if(cp<active->min_unicode || cp>0x10FFFF || (cp>=0xD800&&cp<=0xDFFF))cp=0xFFFD;
     put_char(cp);
    }
    continue;
   }
   active->remaining=0;put_char(0xFFFD);
  }
  if(g_st==ST_GROUND && c>=128) {
   if(c>=0xC2&&c<=0xDF){active->remaining=1;active->unicode=c&31;active->min_unicode=128;}
   else if(c>=0xE0&&c<=0xEF){active->remaining=2;active->unicode=c&15;active->min_unicode=2048;}
   else if(c>=0xF0&&c<=0xF4){active->remaining=3;active->unicode=c&7;active->min_unicode=65536;}
   else put_char(0xFFFD);
  } else vt_feed(c);
 }
}
const cell *lj_vt_row(void *p,int row){active=p;return row<0||row>=ROWS?NULL:view_row(row);}
/* Virtual line: 0 = oldest scrollback line … sb_count-1, then the live grid.
 * Independent of the browse offset (owkTerm's scrollback search). */
int lj_vt_lines(void *p){active=p;return g_sb_count+ROWS;}
const cell *lj_vt_line(void *p,int vi){active=p;if(vi<0||vi>=g_sb_count+ROWS)return NULL;if(vi>=g_sb_count)return g_grid[vi-g_sb_count];return g_sb[(g_sb_head-g_sb_count+vi+2*SB_LINES)%SB_LINES];}
void lj_vt_scroll(void *p,int delta){active=p;sb_scroll(delta);}
static void osc_queue_app(OwkVT *v) {
    if (v->osc_len > 4096) return;                                  /* not a title or a menu item */
    if (v->napp >= (int)(sizeof v->app / sizeof v->app[0])) return;   /* the app is not draining: drop, never grow */
    for (int i = 0; i < v->osc_len; i++) if ((unsigned char)v->osc[i] < 32) return;   /* control bytes: not a message */
    char *m = malloc((size_t)v->osc_len + 1); if (!m) return;
    memcpy(m, v->osc, (size_t)v->osc_len); m[v->osc_len] = 0; v->app[v->napp++] = m;
}
static void osc_dispatch(void) {
    OwkVT *v = active; int n = v->osc_len; v->osc[n] = 0;
    if (!v->osc_over && n >= 2 && ((v->osc[1] == ';' && (v->osc[0] == '0' || v->osc[0] == '2' || v->osc[0] == '7')) ||
                                   (n >= 4 && !memcmp(v->osc, "133;", 4)) || (n >= 5 && !memcmp(v->osc, "7771;", 5)))) { osc_queue_app(v); return; }
    if (v->osc_over || n < 4 || memcmp(v->osc, "52;", 3)) return;
    const char *sel = v->osc + 3, *pay = memchr(sel, ';', (size_t)(n - 3)); if (!pay) return;
    pay++; size_t plen = (size_t)(v->osc + n - pay);
    if (plen == 0 || (plen == 1 && pay[0] == '?')) return;         /* query: never leak the clipboard */
    char *out = malloc(plen / 4 * 3 + 4); if (!out) return;
    size_t o = 0; unsigned acc = 0; int bits = 0, bad = 0;
    for (size_t i = 0; i < plen; i++) { int c = (unsigned char)pay[i]; if (c == '=') break; if (c == '\r' || c == '\n') continue;
        int d = b64v(c); if (d < 0) { bad = 1; break; } acc = (acc << 6) | (unsigned)d; bits += 6; if (bits >= 8) { bits -= 8; out[o++] = (char)((acc >> bits) & 255); } }
    if (bad || o == 0) { free(out); return; }
    for (size_t i = 0; i < o; i++) if (!out[i]) { free(out); return; }   /* NUL inside: not text */
    out[o] = 0; free(v->clip); v->clip = out;
}
char *lj_vt_take_clipboard(void *p){OwkVT *v=p;char *t=v->clip;v->clip=NULL;return t;}
char *lj_vt_take_app_osc(void *p){OwkVT *v=p;if(!v->napp)return NULL;char *t=v->app[0];v->napp--;memmove(v->app,v->app+1,(size_t)v->napp*sizeof v->app[0]);return t;}
int lj_vt_state(void *p,int key){active=p;switch(key){
 case 0:return g_cx;case 1:return g_cy;case 2:return g_cursor_on;
 case 3:return active->alt;case 4:return active->mouse;case 5:return active->sgr_mouse;
 case 6:return active->paste;case 7:return active->app_cursor;case 8:return g_view;
 case 9:return active->sync;
 case 10:return (int)(active->scrolled&0x3fffffff);
 case 11:return active->pixel_mouse;
 default:return 0;}}
const char *lj_vt_reply(void *p){active=p;return active->reply;}
void lj_vt_clear_reply(void *p){((OwkVT*)p)->reply[0]=0;}
void lj_vt_set_pixel_sink(void *p,const lj_vt_pixel_sink *sink){
 OwkVT *v=p; if(sink&&sink->tile_new&&sink->tile_plot&&sink->cell_w>0&&sink->cell_h>0){v->sink=*sink;v->has_sink=1;}
 else {memset(&v->sink,0,sizeof(v->sink));v->has_sink=0;}
}
