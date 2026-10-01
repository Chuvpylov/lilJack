#ifndef LILJACK_C_DASH_H
#define LILJACK_C_DASH_H
#include <json-c/json.h>
/* the dashboard — the home surface: who is working, on what, how far along, on
 * which host, and what the codebase is doing.
 *
 * Draws through the shared primitives only (c_render in the window, c_ansi in
 * the terminal), like c_review — it owns no state, opens no files and never
 * blocks. The data arrives already aggregated in the backend snapshot's
 * `dashboard` key (backend.py Backend.dashboard; contract in
 * docs/codex/reports/2026-09-12-dashboard-backend.md).
 *
 * ⚠ NEVER INVENT A NUMBER. `unavailable` renders as its reason, a missing git
 * count renders as "—", and a task with no progress string shows no bar. A zero
 * bar would claim "measured, and it is nothing", which is the one lie the
 * checklist (G5) forbids.
 *
 * Returns the FULL content height in pixels so the caller can size a scrollbar
 * and clamp `scroll` without knowing anything about the tile. */
int lj_dash_draw(json_object *dashboard, int x, int y, int w, int h, int scroll, int ansi);
#endif
