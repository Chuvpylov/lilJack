/*
 * backends/hui_x11.h — X11 windowed backend for hui
 *
 * Uses the headless software rasterizer (virtual canvas),
 * then scales the result to the X11 window via hui_window.h.
 *
 * Select with: #define HUI_BACKEND_X11 before #include "hui.h"
 * Links: -ldl
 */

#ifndef HUI_X11_H
#define HUI_X11_H

#ifndef HUI_HEADLESS_H
#  define HUI_HEADLESS_NO_FLUSH
#  include "hui_headless.h"
#endif

#ifndef HUI_WINDOW_H
#  include "hui_window.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Called inside hui_begin_frame AFTER snapshotting prev state.
 * Reads fresh input from the X11 backend into hui_g->io. */
static void hui_x11_update_io(void) {
    if (!hui_g) return;
    int sc = hui_win_scale > 0 ? hui_win_scale : 1;
    hui_g->io.mouse_x   = (int16_t)((hui_win_mouse_x - hui_win_off_x) / sc);
    hui_g->io.mouse_y   = (int16_t)((hui_win_mouse_y - hui_win_off_y) / sc);
    hui_g->io.mouse_btn = (uint8_t)hui_win_mouse_btn;
    hui_g->io.scroll_dy = (int16_t)hui_win_scroll;
    hui_win_scroll = 0;  /* consume */
    /* Transfer HUI-indexed key held state */
    memcpy(hui_g->io.keys, hui_win_keys, 256 * sizeof(bool));
    /* Transfer typed chars */
    int n = hui_win_text_len < 31 ? hui_win_text_len : 31;
    memcpy(hui_g->io.text_typed, hui_win_text_buf, (size_t)n);
    hui_g->io.text_typed[n]  = '\0';
    hui_g->io.text_typed_len = n;
    /* Transfer modifier state */
    hui_g->io.mods = (uint8_t)hui_win_mods;
}

#ifdef HUI_IMPLEMENTATION
void hui_backend_flush(const hui_cmd *cmds, uint16_t count,
                       const char *strpool, const float *datapool) {
    /* Rasterize into virtual canvas (hui__fb at original init size) */
    hui__rasterize(cmds, count, strpool, datapool);

    /* Optional overlay pass — app may define g_overlay_hook to draw TTF after
     * rasterization. The &sym guard avoids dereferencing a weak-undefined symbol
     * (whose address is 0) when no app defines it — matches hui_headless.h. */
    { extern __attribute__((weak)) void (*g_overlay_hook)(void);
      if (&g_overlay_hook && g_overlay_hook) g_overlay_hook(); }

    /* Scale-blit virtual canvas → X11 window (nearest-neighbor integer step) */
    hui_win_blit(hui__fb.pixels, hui__fb.w, hui__fb.h);

    /* Clear virtual canvas for next frame */
    memset(hui__fb.pixels, 0,
           (size_t)(hui__fb.w * hui__fb.h) * sizeof(uint32_t));
    /* IO is updated at hui_begin_frame via hui_x11_update_io() */
}
#endif /* HUI_IMPLEMENTATION */

#ifdef __cplusplus
}
#endif

#endif /* HUI_X11_H */
