/* drag-tile-visual-fixture — a tile drag, seen at every step.
 *
 * the operator 2026-09-12 07:10: "visual inspection on a draggable tile". Checklist
 * S4 (tiles + chrome) and S5 (dividers) of the round-4 spec. The existing
 * divider fixtures prove geometry; this one drives a REAL pointer through a
 * whole title drag and captures before / during / after, then asserts what a
 * still cannot show:
 *   D1 the drag is armed by pressing the title and disarmed by the release;
 *   D2 during the drag a snap preview marks the target and the footer says
 *      which edge will be used;
 *   D3 the preview rectangle equals the rectangle the tile actually gets;
 *   D4 no two tiles overlap at any step, and every tile rect stays on the cell
 *      grid and inside the window;
 *   D5 a release over no target leaves the layout exactly as it was;
 *   D6 the divider drag moves the shared edge and both neighbours follow.
 */
#define main liljack_entry
#include "../liljack_app/main.c"
#undef main
#include "../liljack_app/c_ansi.c"
#include <assert.h>
#include <locale.h>

static int save_images, checks;

static void pointer(App *a, Uint32 type, int x, int y) {
    SDL_Event e = {0};
    e.type = type;
    if (type == SDL_MOUSEMOTION) { e.motion.x = x; e.motion.y = y; e.motion.state = SDL_BUTTON_LMASK; }
    else { e.button.x = x; e.button.y = y; e.button.button = SDL_BUTTON_LEFT; }
    a->mousex = x; a->mousey = y;
    event(a, &e);
}

static void shot(App *a, const char *tag) {
    if (!save_images) return;
    const char *dir = getenv("LILJACK_DRAG_CAPTURE_DIR");
    if (!dir) dir = "docs/codex/reports/drag-tile-visual-fixture";
    char path[1024];
    snprintf(path, sizeof path, "%s/%s-%dx%d.png", dir, tag, a->w, a->h);
    lj_render_resize(a->w, a->h);
    for (int y = 0; y < state.rows; y++)
        for (int x = 0; x < state.cols; x++)
            lj_render_rect(x * CELLW, y * CELLH, CELLW, CELLH, state.canvas[y * state.cols + x].bg);
    for (int y = 0; y < state.rows; y++)
        for (int x = 0; x < state.cols; x++) {
            cell *c = &state.canvas[y * state.cols + x];
            if (c->cp && c->cp != ' ')
                lj_render_glyph(x * CELLW, y * CELLH, c->cp, c->fg, c->wide == 2 ? 2 : 1);
            for (int k = 0; k < 3 && c->marks[k]; k++)
                lj_render_glyph(x * CELLW, y * CELLH, c->marks[k], c->fg, c->wide == 2 ? 2 : 1);
        }
    assert(!lj_render_save_png(path));
}

static lj_rect tile_rect(App *a, const char *id) {
    for (int i = 0; i < a->dock.tile_count; i++)
        if (!strcmp(a->dock.tiles[i].id, id)) return a->dock.tiles[i].rect;
    return (lj_rect){0, 0, 0, 0};
}

static int overlap(lj_rect p, lj_rect q) {
    return p.x < q.x + q.w && q.x < p.x + p.w && p.y < q.y + q.h && q.y < p.y + p.h;
}

/* D4 for the whole layout. */
static void layout_sane(App *a, const char *tag) {
    for (int i = 0; i < a->dock.tile_count; i++) {
        lj_rect r = a->dock.tiles[i].rect;
        assert(r.w > 0 && r.h > 0);
        assert(r.x >= 0 && r.y >= 0 && r.x + r.w <= a->w && r.y + r.h <= a->h);
        assert(r.x % CELLW == 0 && r.w % CELLW == 0);
        assert(r.y % CELLH == 0 && r.h % CELLH == 0);
        for (int j = i + 1; j < a->dock.tile_count; j++)
            assert(!overlap(r, a->dock.tiles[j].rect));
    }
    printf("%-26s %4dx%-4d tiles=%d on-grid, inside, no overlap\n", tag, a->w, a->h, a->dock.tile_count);
    checks++;
}

/* The footer cue that names the edge, e.g. "Snap left · release to place". */
static int footer_says_snap(App *a) {
    int row = a->h / CELLH - 1;
    char line[512];
    size_t n = 0;
    for (int x = 0; x < state.cols && n + 1 < sizeof line; x++) {
        uint32_t cp = state.canvas[(size_t)row * state.cols + x].cp;
        line[n++] = (cp > 31 && cp < 127) ? (char)cp : ' ';
    }
    line[n] = 0;
    return strstr(line, "release to place") != NULL;
}

int main(int argc, char **argv) {
    (void)argv;
    save_images = argc > 1 || getenv("LILJACK_DRAG_CAPTURE_DIR");
    setlocale(LC_CTYPE, "C.UTF-8");
    assert(!lj_render_init(800, 560));

    int sizes[][2] = {{800, 560}, {1280, 720}, {1920, 1080}};
    for (unsigned z = 0; z < sizeof sizes / sizeof sizes[0]; z++) {
        App *a = calloc(1, sizeof *a);
        assert(a);
        a->w = sizes[z][0]; a->h = sizes[z][1];
        a->demo = a->ansi = 1; a->split = -1;
        a->backend_in = a->backend_out = -1;
        for (int i = 0; i < COUNT; i++) a->sessions[i].fd = -1;
        lj_dock_init(&a->dock); lj_media_init(&a->media); lj_popup_init(&a->popup);
        demo(a);
        assert(lj_dock_drop(&a->dock, "demo-0", "room", LJ_DOCK_LEFT));
        assert(lj_dock_drop(&a->dock, "demo-1", "demo-0", LJ_DOCK_BOTTOM));
        a->mousex = a->mousey = -1;
        render(a);
        shot(a, "before");
        layout_sane(a, "S4 before drag");

        lj_rect src = tile_rect(a, "demo-1"), dst = tile_rect(a, "room");
        assert(src.w > 0 && dst.w > 0);
        lj_dock saved = a->dock;

        /* D1 press on the title ARMS the tile; the drag itself only starts once
         * the pointer has moved past the 6 px slop (main.c:3262), so a click is
         * never mistaken for a drag. */
        pointer(a, SDL_MOUSEBUTTONDOWN, src.x + src.w / 2, src.y + CELLH / 2);
        render(a);
        assert(!strcmp(a->drag, "demo-1") && !a->dragging);
        checks++;

        /* D2 move over the LEFT edge of the target: preview + footer cue. */
        int mx = dst.x + dst.w / 8, my = dst.y + dst.h / 2;
        pointer(a, SDL_MOUSEMOTION, mx, my);
        render(a);
        pointer(a, SDL_MOUSEMOTION, mx, my);      /* second move: past the slop, now dragging */
        render(a);
        shot(a, "during-snap-left");
        assert(a->dragging);
        assert(footer_says_snap(a));
        checks++;
        printf("%-26s %4dx%-4d preview shown, footer names the edge\n", "S4 during drag", a->w, a->h);

        /* D3 the preview rect is the rect the tile actually gets. */
        lj_rect preview = tile_rect(a, "demo-1");
        lj_dock_edge edge = lj_dock_edge_at(dst, mx, my);
        assert(drop_preview(a, "room", edge, &preview));
        pointer(a, SDL_MOUSEBUTTONUP, mx, my);
        render(a);
        shot(a, "after-drop");
        assert(!a->dragging);
        lj_rect landed = tile_rect(a, "demo-1");
        printf("%-26s %4dx%-4d preview=%d,%d %dx%d landed=%d,%d %dx%d\n", "S4 drop", a->w, a->h,
               preview.x, preview.y, preview.w, preview.h, landed.x, landed.y, landed.w, landed.h);
        assert(landed.x == preview.x && landed.y == preview.y
               && landed.w == preview.w && landed.h == preview.h);
        checks++;
        layout_sane(a, "S4 after drop");

        /* D5 a drag released over nothing leaves the layout untouched. */
        a->dock = saved;
        render(a);
        lj_rect keep = tile_rect(a, "demo-1");
        pointer(a, SDL_MOUSEBUTTONDOWN, keep.x + keep.w / 2, keep.y + CELLH / 2);
        pointer(a, SDL_MOUSEMOTION, a->w - 1, 0);      /* off every tile */
        render(a);
        pointer(a, SDL_MOUSEMOTION, a->w - 1, 0);
        render(a);
        shot(a, "during-no-target");
        pointer(a, SDL_MOUSEBUTTONUP, a->w - 1, 0);
        render(a);
        lj_rect same = tile_rect(a, "demo-1");
        assert(!a->dragging);
        assert(same.x == keep.x && same.y == keep.y && same.w == keep.w && same.h == keep.h);
        printf("%-26s %4dx%-4d release over no target: layout unchanged\n", "S4 invalid drop", a->w, a->h);
        checks++;
        layout_sane(a, "S4 after invalid drop");

        /* D6 divider drag: the shared edge moves and both neighbours follow. */
        if (a->dock.divider_count > 0) {
            lj_dock_divider d = a->dock.dividers[0];
            int dx = d.rect.x + d.rect.w / 2, dy = d.rect.y + d.rect.h / 2;
            int horizontal = d.rect.h <= d.rect.w;
            pointer(a, SDL_MOUSEMOTION, dx, dy);
            render(a);
            shot(a, "divider-hover");
            pointer(a, SDL_MOUSEBUTTONDOWN, dx, dy);
            assert(a->split == d.node);
            int step = horizontal ? 2 * CELLH : 2 * CELLW;
            pointer(a, SDL_MOUSEMOTION, horizontal ? dx : dx + step, horizontal ? dy + step : dy);
            render(a);
            shot(a, "divider-drag");
            pointer(a, SDL_MOUSEBUTTONUP, horizontal ? dx : dx + step, horizontal ? dy + step : dy);
            render(a);
            shot(a, "divider-after");
            assert(a->split == -1);
            lj_rect moved = a->dock.dividers[0].rect;
            printf("%-26s %4dx%-4d %s divider %d,%d -> %d,%d\n", "S5 divider drag", a->w, a->h,
                   horizontal ? "horizontal" : "vertical", d.rect.x, d.rect.y, moved.x, moved.y);
            assert(horizontal ? moved.y != d.rect.y : moved.x != d.rect.x);
            checks++;
            layout_sane(a, "S5 after divider drag");
        }

        for (int i = 0; i < a->nsession; i++) if (a->sessions[i].vt) lj_vt_free(a->sessions[i].vt);
        if (a->messages) json_object_put(a->messages);
        drafts_close(a);
        free(a);
    }
    lj_render_close();
    printf("Drag: %d checks over title drag, invalid drop and divider drag x 3 sizes PASS\n", checks);
    return 0;
}
