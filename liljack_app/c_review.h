#ifndef LILJACK_C_REVIEW_H
#define LILJACK_C_REVIEW_H
#include <json-c/json.h>
/* App review panel: session TODOs with completion, per-agent progress, git
 * history and recorded agent fit.
 *
 * Draws through the shared primitives only (c_render in the window, c_ansi in
 * the terminal), so it owns no state, opens no files and never blocks — the
 * snapshot arrives already gathered by review_panel.py.
 *
 * Returns the FULL content height in pixels, which is what lets the caller size
 * a scrollbar and clamp `scroll` without the renderer knowing anything about
 * the tile. Content above/below the box is clipped, never wrapped around.
 *
 * ⚠ A section whose data is missing prints its REASON, never a zero. A panel
 * that silently shows 0% looks like progress that did not happen. */
int lj_review_draw(json_object *snapshot, int x, int y, int w, int h,
                   int scroll, int ansi);
#endif
