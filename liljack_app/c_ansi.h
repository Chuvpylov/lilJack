#ifndef LILJACK_C_ANSI_H
#define LILJACK_C_ANSI_H
#include <SDL.h>
#include <stdint.h>
/* This module owns stdin termios/flags and stdout alternate screen while open.
 * Single UI thread. Pixel coordinates map to terminal cells of 10x20 pixels.
 * No SDL video initialization is needed. Open returns 1, or 0 with a diagnostic.
 * Paste is one owned NUL-terminated UTF-8 string (maximum 65536 bytes) returned
 * as SDL_USEREVENT with code LJ_ANSI_PASTE_CODE and user.data1. Caller must free.
 * Oversized/incomplete pastes are discarded, never emitted as executable keys.
 * SDL_USEREVENT/LJ_ANSI_ERROR_CODE carries an owned diagnostic string likewise.
 * Text events always carry whole UTF-8 codepoints, never embedded Enter keys.
 * Key modifiers are in event.key.keysym.mod; mouse/text modifiers are available
 * through lj_ansi_modifiers().
 */
#define LJ_ANSI_PASTE_CODE 0x4c4a0001
#define LJ_ANSI_ERROR_CODE 0x4c4a0002
/* Smallest terminal the workspace lays out completely, in cells. 80x28 cells is
 * 800x560 px, which is exactly the minimum main.c already clamps --width and
 * --height to; the window path enforces it, the terminal path cannot. Measured
 * below it: the room composer and Send are silently dropped (80x27 and 60x30
 * lose them, 80x28 and 70x30 keep them). lj_ansi_present therefore REPORTS the
 * shortfall on the last row rather than clamping - a terminal cannot be resized
 * from here, and a clamped layout would be a different lie. */
#define LJ_ANSI_MIN_COLS 80
#define LJ_ANSI_MIN_ROWS 28
/* Input-only hint: in the message composer, unframed text bursts (32+ bytes
 * or text plus newline) are collected until 25ms idle and emitted as paste.
 * A lone Enter stays a key. Bracketed paste is exact and needs no heuristic. */
void lj_ansi_message_input(int enabled);
int lj_ansi_open(int *pixelw,int *pixelh);
void lj_ansi_close(void);
/* Clipboard writes are requests only: the host may reject OSC 52. */
int lj_ansi_copy(const char *text);
int lj_ansi_mouse(int enabled);
/* Testing/diagnostics: treat SGR mouse reports as pixel coordinates (?1016). */
void lj_ansi_pixel_mouse(int enabled);
void lj_ansi_begin(int pixelw,int pixelh);
/* Reserved gutter/rules: sixel uses 1 host pixel idle, 3 when hot; no block
 * glyphs. Terminals without sixel retain the aspect-matched block fallback.
 * Later overlays and text own their cells and cover the stroke. */
void lj_ansi_separator(int x,int y,int w,int h,uint32_t rgb,int vertical);
void lj_ansi_separator_hot(int x,int y,int w,int h,uint32_t rgb,int vertical,int hot);
void lj_ansi_rect(int x,int y,int w,int h,uint32_t rgb);
void lj_ansi_text(int x,int y,const char *utf8,uint32_t rgb,int maxw);
void lj_ansi_glyph(int x,int y,uint32_t cp,uint32_t rgb,int cells);
/* Put a decorative U+2022 bullet on a background/rule cell only. Never replaces
 * text, text spaces, combining marks or either half of a wide glyph. Returns
 * 1 when drawn, 0 when clipped/covered. begin()/rect() reset cell ownership. */
int lj_ansi_dot(int x,int y,uint32_t rgb);
#define LJ_BORDER_TICK_MS 50u
/* The lead ring's pattern, in PIXELS: a 2x2 dot every 6 px (2 on, 4 off),
 * marching one pixel per tick. the operator set these live on 2026-09-12; they are
 * absolute, not derived from the cell grid, so every tile shows one pattern. */
#define LJ_BORDER_DOT 2
#define LJ_BORDER_GAP 4
/* Per-edge cell-derived anchors, one-pixel dots outside the text frame in
 * available gutter. Phase in LJ_BORDER_TICK_MS ticks; colours advance every
 * 10 ticks. The ANSI dots remain on preparation failure. Screen boundaries
 * clip the gutter and occupied cells always win. No interior space reserved. */
int lj_ansi_border(int x,int y,int w,int h,uint32_t gold,uint32_t blue,uint32_t phase);
/* Re-emit only the sparse border when its independent phase changes. */
int lj_ansi_border_tick(uint32_t phase);
/* Cell-native visual effects, off by default. The window path dims raw pixels
 * (main.c, guarded !ansi) which a character grid has none of, so --effects was
 * silently inert in the terminal. This dims the BACKGROUND of alternate rows -
 * a scanline - and never touches cp or marks, so no glyph can be lost. */
void lj_ansi_effects(int enabled);
/* Terminal cursor as REVERSE VIDEO on the cell at pixel (x,y), never as a rect.
 * ⚠ A thin cursor underline drawn through lj_ansi_rect hit the h<10 branch and
 * became a ─ line glyph that REPLACED the character under the cursor, and — at
 * any grid origin not a multiple of the cell width — did it to the next cell
 * too. That is the "double blue block covering what it selects +1 char".
 * Swapping fg/bg cannot lose a glyph. cells is 1 or 2 (a wide CJK cell). */
void lj_ansi_cursor(int x, int y, int cells);
/* ── graphical regions (sixel) ──────────────────────────────────────────────
 * Present the ARGB source image `src` (sw x sh, tightly packed, top-left
 * origin) inside the lilJack pixel rect (x,y,w,h), on top of the cells already
 * drawn there. Returns 1 when the region was accepted for this frame, 0 when
 * the terminal cannot show it.
 *
 * ⚠ THE CELL PATH IS THE FALLBACK, AND IT IS WHY THIS RETURNS 0 INSTEAD OF
 * DRAWING SOMETHING. A terminal without sixel renders the escape as garbage
 * sprayed across the whole screen — far worse than an honest refusal. So the
 * caller draws its cell-based placeholder FIRST and calls this after: when
 * sixel is unavailable this is a no-op and the placeholder is simply what the
 * user sees. Support is probed once, via DA1, at open.
 *
 * ⚠ THE RECT IS IN lilJACK PIXELS; WHAT IS HONOURED IS THE CELLS IT COVERS.
 * lilJack lays out on a fixed 10x20 grid, but the host terminal's cell is
 * whatever its font makes it — emitting an image at lilJack's pixel size lands
 * at the wrong size in every terminal whose font is not 10x20. The image is
 * therefore SCALED to (cols x terminal-cell-width) by (rows x terminal-cell-
 * height), so it occupies exactly the cells the layout gave it. The terminal's
 * cell size is queried (CSI 16 t, then CSI 14 t) at open and falls back to
 * 10x20 only when the terminal answers neither.
 *
 * ⚠ Bandwidth, not encoding, is the constraint: a full frame measured 268,942
 * bytes. Regions are damage-tracked (an unchanged one is not re-sent), rate-
 * limited per region, and dropped rather than truncated if they exceed the
 * byte cap — a stalled pty is worse than a missing frame. */
/* Copies source at enqueue (4 MiB/slot, 8 slots); caller may reuse/free it on return; failure queues nothing. */
int lj_ansi_image(int x, int y, int w, int h,
                  const uint32_t *src, int sw, int sh);
/* The image just queued by lj_ansi_image changed ONLY inside this SOURCE-pixel
 * rect since the previous frame (live drawing). The presenter then re-sends
 * just the covering cells as a small sixel patch instead of the whole image,
 * and sends it even when the sampled content hash cannot see a thin line.
 * Several calls union. No effect on a moved/resized or first-shown image. */
void lj_ansi_image_damage(int sx, int sy, int sw, int sh);
/* Before painting an opaque overlay: flatten intersecting queued images into
 * glyph cells so their late sixel presentation cannot cover its chrome. */
void lj_ansi_cover(int x, int y, int w, int h);
/* 1 when the host terminal advertises sixel (DA1 attribute 4). Callers use it
 * to decide whether to spend effort producing pixels at all. */
int lj_ansi_images_available(void);
/* 1 when the host advances two cells for an ambiguous-width icon + U+FE0F (measured at open; LJ_ANSI_VS16 overrides). */
int lj_ansi_vs16_wide(void);
/* The host terminal's measured cell size in pixels; 10x20 when unanswered. */
void lj_ansi_cell_pixels(int *w, int *h);
int lj_ansi_present(void); /* 1 success, 0 write/allocation failure */
/* LILJACK_TRACE_OVERLAYS=1 logs present/emitting ticks to redirected stderr only.
 * Supply the current framed lead after begin(); logs contain no draft text. */
void lj_ansi_trace_lead(const char *id,int x,int y,int w,int h);
/* One reserved right-edge cell column: pixel thumb on sixel, block cells
 * otherwise. offset/thumb are pixels within h; opacity is 0..255. */
void lj_ansi_scrollbar(int x,int y,int h,int offset,int thumb,int opacity);
int lj_ansi_poll(SDL_Event *event); /* 1 event; 0 no event */
SDL_Keymod lj_ansi_modifiers(void); /* latest input modifiers; use instead of SDL_GetModState */
#endif
