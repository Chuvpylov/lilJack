/* owkterm_vt.h — public ABI for the owkterm-derived terminal emulator.
 *
 * Single UI thread. Instance state is opaque; every function takes the void*
 * handle returned by lj_vt_new. The cell layout (24 bytes: ch,fg,bg,marks[3])
 * is stable and is what the ctypes adapter and the C frontend (main.c/render)
 * both bind to.
 *
 * Build:  gcc -shared -fPIC -O2 -std=c11 owkterm_vt.c -o liblj_vt.so
 * The translation unit is self-contained (libc + <wchar.h>/<locale.h> only).
 */
#ifndef OWKTERM_VT_H
#define OWKTERM_VT_H

#include <stdint.h>

#define LJ_VT_MAXCOLS 300
#define LJ_VT_MAXROWS 120

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t ch;            /* Unicode code point; 0 == wide-char continuation */
    uint32_t fg;            /* 0..7 base, +8 bright, or 0x1000000|RGB truecolor */
    uint32_t bg;
    uint32_t marks[3];      /* combining marks attached to ch */
} lj_cell;

/* lj_vt_state() keys */
enum {
    LJ_VT_CX          = 0,
    LJ_VT_CY          = 1,
    LJ_VT_CURSOR_ON   = 2,
    LJ_VT_ALT         = 3,
    LJ_VT_MOUSE       = 4,
    LJ_VT_SGR_MOUSE   = 5,
    LJ_VT_PIXEL_MOUSE = 11,   /* ?1016: SGR reports carry PIXEL coordinates (lilJack canvas drawing) */
    LJ_VT_PASTE       = 6,
    LJ_VT_APP_CURSOR  = 7,
    LJ_VT_VIEW        = 8,     /* scrollback browse offset (>=0) */
    LJ_VT_SYNC        = 9      /* synchronized output (DECSET 2026): 1 between ESC[?2026h and
                                * ESC[?2026l — the presenter must NOT show the grid while set, so a
                                * frame that arrives across several pty reads commits as one
                                * (lilJack's header: text + three graph images). Callers keep a
                                * timeout so a sender that dies inside a fence cannot freeze them. */,
    LJ_VT_SCROLLED    = 10     /* rows that scrolled off the top of the normal screen since start
                                * (monotonic, wraps at 2^30): anchor a selection to text, not rows */
};

/* Allocate an instance (cols/rows clamped to [1,LJ_VT_MAXCOLS]/[1,LJ_VT_MAXROWS]). */
void *lj_vt_new(int cols, int rows);
void  lj_vt_free(void *p);

/* Resize. Preserves scrollback across a width change (lines truncate on
 * narrow, blank-pad on wide; no reflow). Sends no signal — the caller owns
 * TIOCSWINSZ against the pty. */
void  lj_vt_resize(void *p, int cols, int rows);

/* Feed raw pty/UTF-8 bytes through the VT parser. Bounds-checked against
 * hostile CSI and malformed UTF-8 (no allocation, no OOB access). */
void  lj_vt_feed(void *p, const unsigned char *buf, int len);

/* The rendered row for the current scrollback view (NULL if row out of range). */
const lj_cell *lj_vt_row(void *p, int row);

/* Browse scrollback: delta>0 scrolls up, delta<0 down. Clamped. */
void  lj_vt_scroll(void *p, int delta);

int  lj_vt_state(void *p, int key);

/* ── pixel-level sixel (owkTerm native app) ───────────────────────────────
 * Without a sink, sixel decodes into half blocks (▀, 2 pixel rows per cell:
 * the resolution lilJack's tiles get). With a sink the VT hands every pixel
 * to the app: a cell that receives pixels gets ch=U+2580, fg|=LJ_VT_FG_PIXELS
 * and marks[1]=tile id from tile_new; the app owns the tile store (cell_w x
 * cell_h pixels per tile). The id travels with the cell through scroll,
 * scrollback and resize, and is dropped when text overwrites the cell. The
 * half-block colours are kept up to date as a fallback. A sink also makes
 * DA1 advertise sixel (parameter 4). */
#define LJ_VT_FG_PIXELS 0x2000000u
typedef struct {
    void    *user;
    int      cell_w, cell_h;
    uint32_t (*tile_new)(void *user, uint32_t bg);              /* nonzero id; bg is an lj_cell colour */
    void     (*tile_plot)(void *user, uint32_t tile, int px, int py, uint32_t rgb);
    /* Optional. A NEW sixel that touches a cell which already owns a tile
     * resets that tile to its ground before plotting, so a transparent
     * overlay re-emitted at the same place (lilJack's marching ring, its
     * header graphs) replaces the old pixels instead of piling on top of
     * them — the "imprints accumulate" defect (the operator, 2026-09-12). NULL =
     * the VT allocates a fresh tile instead (id churn on the ring buffer). */
    void     (*tile_reset)(void *user, uint32_t tile, uint32_t bg);
    /* Optional. Called once at the end of each sixel image (ST/BEL). A sink that
     * double-buffers (plots into scratch tiles during the image, commits them
     * here) makes every image ATOMIC to the viewer: a pty read that ends in the
     * middle of an image never shows a half-wiped tile — the "graphs flicker /
     * tear" the operator saw (codex, 2026-09-13: 337/375 cut points exposed partial state). */
    void     (*image_end)(void *user);
} lj_vt_pixel_sink;
void lj_vt_set_pixel_sink(void *p, const lj_vt_pixel_sink *sink);   /* NULL = half blocks */
/* Enumerate every tile id still referenced by a cell — visible grid, the
 * saved (other) screen and the scrollback — so the app can recycle only
 * unreferenced tiles. Recycling a referenced id showed a NEW image inside a
 * stale cell ("mirroring of active elements in empty parts", the operator 2026-09-12). */
void lj_vt_tiles(void *p, void (*cb)(void *user, uint32_t id), void *user);
const char *lj_vt_reply(void *p);       /* pending DSR/DA reply (static, '\0' if none) */
/* OSC 52 clipboard set from the child (lilJack's copy). Returns the decoded text
 * once (malloc'd, caller frees) and clears it; NULL when nothing is pending. */
char *lj_vt_take_clipboard(void *p);
/* Next queued app message from the child, or NULL: the OSC body without the
 * terminator, e.g. "0;title", "7;file://host/path", "7771;menu;add;id;label[;keys]".
 * malloc'd, caller frees. Bounded queue (32); overflow drops, never blocks. */
char *lj_vt_take_app_osc(void *p);
/* Whole history as virtual lines: 0 = oldest scrollback line, the last ROWS are
 * the live grid. lj_vt_lines is the count; lj_vt_line returns a row or NULL. */
int lj_vt_lines(void *p);
const lj_cell *lj_vt_line(void *p, int vi);
void lj_vt_clear_reply(void *p);

#ifdef __cplusplus
}
#endif

#endif /* OWKTERM_VT_H */
