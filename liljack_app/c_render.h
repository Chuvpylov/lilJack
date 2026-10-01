/* c_render.h — lilJack's C rendering module.
 *
 * Owner: claude (delegated by codex, 2026-09-08). Paints into HUI's headless
 * ARGB8888 framebuffer; codex's SDL layer presents it. No GPU.
 *
 * Geometry: text cell 10 x line 20, TOP-LEFT coordinates throughout.
 *
 * Font fallback chain, resolved per codepoint:
 *     mono (Latin/Cyrillic) -> CJK -> colour emoji -> tofu box
 * A missing font file is SKIPPED, not fatal: init still succeeds with whatever
 * faces opened, and if EVERY font is absent it falls back to HUI's built-in
 * 8x8 ASCII atlas rather than failing — Latin still reads, non-ASCII draws
 * tofu, so the degradation is visible. lj_render_init never fails on fonts, and a codepoint with no glyph anywhere draws a hollow box, so a
 * missing font is VISIBLE rather than silent.
 *
 * ⚠ COLOUR EMOJI IGNORES `rgb`. NotoColorEmoji is CBDT/CBLC — a colour BITMAP
 * face, not scalable outlines. Its artwork carries its own colours, so tinting
 * would destroy it, and it ships fixed strikes only (rendered by selecting the
 * nearest strike and downscaling). Every other face tints normally.
 *
 * ⚠ `rgb` is 0xRRGGBB. The top byte is ignored; pixels composite opaque.
 * ⚠ lj_render_pixels() is INVALIDATED by lj_render_resize()/lj_render_close().
 *    Re-fetch after any resize.
 * ⚠ Single UI thread only (same constraint as owkterm_vt.c). No locking.
 */
#ifndef LJ_C_RENDER_H
#define LJ_C_RENDER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LJ_CELL_W   10
#define LJ_LINE_H   20

/* Runtime font configuration (owkTerm). Call BEFORE lj_render_init; lilJack's
 * own UI never calls it and keeps the 10x20 grid. mono_path/emoji_path/cjk_path
 * are tried before the built-in candidates (NULL = built-ins only). px > 0
 * rasterises the mono face at that pixel size and DERIVES the grid from the
 * face: cell width = advance of 'M', line height = the face's line spacing,
 * baseline from its ascender. px == 0 keeps the compile-time grid. */
void      lj_render_set_font(const char *mono_path, const char *emoji_path,
                             const char *cjk_path, int px);
/* Multiplies the face-derived line height (1.0 = the face's own spacing).
 * Applied by lj_render_init after set_font; ignored for the compile-time grid. */
void      lj_render_set_line_scale(double scale);
int       lj_render_cell_w(void);             /* runtime grid (== LJ_CELL_W unless set_font) */
int       lj_render_line_h(void);

/* 0 on success; negative only if NO face could be opened at all. */
int       lj_render_init(int width, int height);
void      lj_render_resize(int w, int h);
void      lj_render_rect(int x, int y, int w, int h, uint32_t rgb);
/* Draws UTF-8 left-to-right from (x,y); stops before exceeding max_width
 * pixels (max_width <= 0 means unbounded). Advances LJ_CELL_W per column,
 * doubled for wide (CJK) codepoints. */
void      lj_render_text(int x, int y, const char *utf8, uint32_t rgb, int max_width);
/* One codepoint clipped to (LJ_CELL_W * cells) x LJ_LINE_H. cells is 1 or 2. */
void      lj_render_glyph(int x, int y, uint32_t cp, uint32_t rgb, int cells);
/* Rounded panel fill — the "island" look, not a box grid. radius is clamped to
 * half the shorter side; radius 0 is an ordinary rect. Corners are
 * anti-aliased, so a panel edge against the dark ground does not stair-step. */
void      lj_render_panel(int x, int y, int w, int h, int radius, uint32_t rgb);
/* Text at an integer scale. Glyphs are RE-RASTERISED at the larger pixel size
 * (not upscaled), so a title stays sharp. scale 1 == lj_render_text. */
void      lj_render_text_scaled(int x, int y, const char *utf8, uint32_t rgb,
                                int max_width, int scale);

/* Trim N pixels off the rasterised TEXT height, leaving the layout cell alone.
 * LJ_CELL_W / LJ_LINE_H remain the grid - advance, wrapping and hit-testing are
 * untouched - so a terminal pane can read one point smaller than the chrome and
 * still line up with its own VT grid. 0 restores the default; clamped to
 * LJ_LINE_H/2 and never rasterised below 6px. Emoji stay cell-sized. */
void      lj_render_text_trim(int px);

uint32_t *lj_render_pixels(void);
void      lj_render_close(void);

/* Introspection + evidence (used by the test harness, safe in production). */
int       lj_render_width(void);
int       lj_render_height(void);
int       lj_render_faces(void);              /* how many faces actually opened */
int       lj_render_has_codepoint(uint32_t cp);
int       lj_render_cells_for(uint32_t cp);   /* 1 or 2, wcwidth-based */
int       lj_render_cells_measure(uint32_t cp); /* 0, 1 or 2 — sizing a string, not advancing */
int       lj_render_save_ppm(const char *path);
int       lj_render_save_png(const char *path);
/* Save a rectangle of the framebuffer (clipped to it) as PNG; scale 2 writes
 * each pixel as a 2x2 block so a small region reads at a glance. 0 on success. */
int       lj_render_save_png_region(const char *path, int x, int y, int w, int h, int scale);

#ifdef __cplusplus
}
#endif
#endif /* LJ_C_RENDER_H */
