#ifndef LJ_SIXEL_H
#define LJ_SIXEL_H
/* c_sixel.h — RGB frame → sixel bytes, the terminal image ENCODER.
 *
 * Owner: deepseek (2026-09-10, claude msg tui-sixel-out). This is the
 * "hacky-but-solid half": lilJack renders every pixel of the whole interface
 * into a HUI ARGB framebuffer (lj_render_pixels); sixel makes the terminal a
 * SECOND presenter of that same buffer. This module turns an ARGB damage rect
 * into a DEC sixel sequence (DCS … ST). The PRESENTER (tui-sixel-present,
 * claude) owns damage tracking, the frame loop, and the cell-path fallback.
 *
 * ⚠ BANDWIDTH IS THE PROBLEM, NOT ENCODING. A full 1400×900 frame re-sent at
 * 8fps is megabytes a second down a pty. So the encoder takes a DAMAGE RECT,
 * not a whole frame, and the caller measures lj_sixel_encode's byte count to
 * decide whether sixel is the default presenter or stays opt-in.
 *
 * ⚠ SUPPORT IS DETECTED, NEVER ASSUMED. A terminal without sixel renders the
 * escape as garbage across the whole screen — worse than an honest refusal.
 * lj_sixel_supported() queries DA1 and reports the sixel attribute (4). Inside
 * tmux the caller must wrap the sequence (and the DA1 query) in a passthrough
 * (ESC P tmux ; ESC … ESC \\) — that wrapping belongs to the presenter, which
 * knows whether it is on a tmux pty; the bytes this module emits are the raw
 * terminal form.
 *
 * ⚠ SIZE CAP. The encoder writes into a caller-supplied buffer and returns -1
 * the moment the output would exceed `cap`, so a huge frame cannot stall the
 * write — the caller splits the rect instead. Allocation is one small per-band
 * scratch buffer, freed before return.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Encode the ARGB damage rect (x, y, w, h) of `frame` (fw × fh, tightly packed,
 * top-left origin) as a sixel sequence. Returns bytes written (excluding NUL),
 * or -1 when `cap` is too small for the rect. Never NUL-terminates beyond cap. */
int lj_sixel_encode(const uint32_t *frame, int fw, int fh,
                    int x, int y, int w, int h,
                    char *out, int cap);

/* Parse a DA1 response (CSI ? Ps ; … ; Ps c) and report whether attribute 4
 * (sixel) is present. 1 = sixel, 0 = not (or not a DA1 response). */
int lj_sixel_da1_has_sixel(const char *response, int len);

/* Query the terminal on `fd`: write DA1 (CSI c), read the reply with a short
 * timeout, parse it. 1 = sixel supported, 0 = unsupported or unknown. Blocks at
 * most ~200ms. */
int lj_sixel_supported(int fd);

#ifdef __cplusplus
}
#endif
#endif /* LJ_SIXEL_H */
