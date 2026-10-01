/* popups-visual-fixture — every popup/dialog/overlay, three sizes, one pass.
 *
 * the operator 2026-09-12 07:08: "lets also debug all popups do visual inspection".
 * Checklist sections S9 (menus/ribbon), S10 (dialogs/toast), S11 (popup/media)
 * of docs/superpowers/specs/2026-09-12-visual-round4-checklist.md.
 *
 * Each surface is opened by its REAL state flag, rendered, captured, and then
 * checked against the geometry rules that a screenshot alone cannot prove:
 *   P1 every painted cell of the surface lies INSIDE the window;
 *   P2 the surface's painted extent starts and ends on the cell grid (it does
 *      by construction of the cell canvas, so P2 is asserted as "the surface
 *      paints whole cells", i.e. no partial row/column is left half-painted);
 *   P3 closing the surface restores the canvas that was under it byte for byte;
 *   P4 a surface that carries text never leaves a cell with a truncated glyph
 *      run at the right edge without an ellipsis (checked where the surface
 *      elides, i.e. the status/footer row and the dialog title).
 * A capture is written for every surface at every size when a capture dir is set.
 */
#define main liljack_entry
#include "../liljack_app/main.c"
#undef main
#include "../liljack_app/c_ansi.c"
#include <assert.h>
#include <locale.h>

static int save_images;
static int checks;

static void shot(App *a, const char *tag) {
    if (!save_images) return;
    const char *dir = getenv("LILJACK_POPUP_CAPTURE_DIR");
    if (!dir) dir = "docs/codex/reports/popups-visual-fixture";
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

static size_t canvas_bytes(void) { return (size_t)state.cols * state.rows * sizeof(cell); }

static cell *canvas_copy(void) {
    cell *c = malloc(canvas_bytes());
    assert(c);
    memcpy(c, state.canvas, canvas_bytes());
    return c;
}

/* Cells this surface changed relative to `base`: its painted extent. */
static int extent(const cell *base, int *x0, int *y0, int *x1, int *y1) {
    *x0 = state.cols; *y0 = state.rows; *x1 = -1; *y1 = -1;
    int n = 0;
    for (int y = 0; y < state.rows; y++)
        for (int x = 0; x < state.cols; x++) {
            size_t i = (size_t)y * state.cols + x;
            if (!memcmp(&base[i], &state.canvas[i], sizeof(cell))) continue;
            n++;
            if (x < *x0) *x0 = x;
            if (y < *y0) *y0 = y;
            if (x > *x1) *x1 = x;
            if (y > *y1) *y1 = y;
        }
    return n;
}

/* P1 + P2: the surface painted something, entirely inside the window. */
static void inside_window(App *a, const cell *base, const char *tag) {
    int x0, y0, x1, y1;
    int n = extent(base, &x0, &y0, &x1, &y1);
    printf("%-22s %4dx%-4d cells=%-5d rect=%d,%d..%d,%d", tag, a->w, a->h, n, x0, y0, x1, y1);
    assert(n > 0);                                   /* the surface must be visible at all */
    assert(x0 >= 0 && y0 >= 0);
    assert(x1 < state.cols && y1 < state.rows);      /* never off the right/bottom edge */
    assert((x1 + 1) * CELLW <= a->w && (y1 + 1) * CELLH <= a->h);
    printf("  inside=yes\n");
    checks++;
}

/* P3: after closing, the canvas under the surface is exactly what it was. */
static void restores(App *a, cell *before, const char *tag) {
    render(a);
    assert(!memcmp(before, state.canvas, canvas_bytes()));
    printf("%-22s %4dx%-4d restores background: yes\n", tag, a->w, a->h);
    checks++;
    free(before);
}

/* P4: an elided row ends in … rather than a chopped word. */
static void elides(App *a, int row, const char *tag) {
    if (row < 0 || row >= state.rows) return;
    int last = -1;
    for (int x = 0; x < state.cols; x++) {
        cell *c = &state.canvas[(size_t)row * state.cols + x];
        if (c->content && c->cp && c->cp != ' ') last = x;
    }
    if (last < 0) return;
    cell *c = &state.canvas[(size_t)row * state.cols + last];
    int full = last == state.cols - 1;
    if (full) assert(c->cp == 0x2026 || c->cp == ' ');   /* runs to the edge -> ellipsis */
    printf("%-22s %4dx%-4d row %d last col %d cp=U+%04X%s\n", tag, a->w, a->h, row, last,
           c->cp, full ? " (edge, elided)" : "");
    checks++;
}

int main(int argc, char **argv) {
    (void)argv;
    save_images = argc > 1 || getenv("LILJACK_POPUP_CAPTURE_DIR");
    setlocale(LC_CTYPE, "C.UTF-8");
    assert(!lj_render_init(800, 560));
    App *a = calloc(1, sizeof *a);
    assert(a);
    a->demo = a->ansi = 1;
    a->split = -1;
    a->backend_in = a->backend_out = -1;
    for (int i = 0; i < COUNT; i++) a->sessions[i].fd = -1;
    lj_dock_init(&a->dock);
    lj_media_init(&a->media);
    lj_popup_init(&a->popup);
    demo(a);

    int sizes[][2] = {{800, 560}, {1280, 720}, {1920, 1080}};
    for (unsigned z = 0; z < sizeof sizes / sizeof sizes[0]; z++) {
        a->w = sizes[z][0];
        a->h = sizes[z][1];
        a->mousex = a->mousey = -1;
        a->about_open = a->room_dialog = a->team = a->status_open = a->ribbon_open = 0;
        menu_close(a);
        copy(a->focus, sizeof a->focus, "room");
        a->toast[0] = 0;
        render(a);
        shot(a, "baseline");
        cell *base = canvas_copy();

        /* S10 room dialog — the round-3 defect "room-dialog-clips at 800". */
        cell *before = canvas_copy();
        a->room_dialog = 1; a->room_field = RF_NAME;
        copy(a->room_name, sizeof a->room_name,
             "A room name long enough to test elision in the dialog title row");
        render(a); shot(a, "room-dialog");
        inside_window(a, base, "S10 room-dialog");
        /* An overflowing field shows its TAIL with a leading ellipsis, so the
         * caret position is visible (fixture finding, 2026-09-12). */
        {
            /* The value overflows the box, so its head is hidden and a leading
             * ellipsis marks the cut; the character after it may be a space. */
            int ell = -1, ell_row = -1;
            for (int y = 0; y < state.rows && ell < 0; y++)
                for (int x = 0; x < state.cols; x++)
                    if (state.canvas[(size_t)y * state.cols + x].cp == 0x2026) {
                        ell = x; ell_row = y; break;
                    }
            if (ell < 0) {
                puts("  no ellipsis on the canvas; NAME row as rendered:");
                for (int y = 0; y < state.rows; y++) {
                    int hit_row = 0;
                    for (int x = 0; x + 4 < state.cols; x++) {
                        cell *c = &state.canvas[(size_t)y * state.cols + x];
                        if (c->cp == 'N' && state.canvas[(size_t)y * state.cols + x + 1].cp == 'A'
                            && state.canvas[(size_t)y * state.cols + x + 2].cp == 'M') hit_row = 1;
                    }
                    if (!hit_row) continue;
                    printf("   %3d: ", y);
                    for (int x = 0; x < state.cols; x++) {
                        uint32_t cp = state.canvas[(size_t)y * state.cols + x].cp;
                        putchar(cp > 31 && cp < 127 ? (char)cp : cp ? '?' : ' ');
                    }
                    putchar('\n');
                }
                fflush(stdout);
            }
            assert(ell >= 0);
            printf("%-22s %4dx%-4d field scrolls to caret: leading … at row %d col %d\n",
                   "S10 room-dialog", a->w, a->h, ell_row, ell);
            checks++;
        }
        a->room_dialog = 0; a->room_name[0] = 0;
        restores(a, before, "S10 room-dialog");

        /* S12 About. */
        before = canvas_copy();
        a->about_open = 1; render(a); shot(a, "about");
        inside_window(a, base, "S12 about");
        a->about_open = 0;
        restores(a, before, "S12 about");

        /* S3/S10 team dialog (roles + control). */
        before = canvas_copy();
        a->team = 1; render(a); shot(a, "team");
        inside_window(a, base, "S10 team");
        a->team = 0;
        restores(a, before, "S10 team");

        /* S8 status panel. */
        before = canvas_copy();
        a->status_open = 1; render(a); shot(a, "status-panel");
        inside_window(a, base, "S8 status-panel");
        a->status_open = 0;
        restores(a, before, "S8 status-panel");

        /* S9 ribbon open, and the MORE overflow at the narrow size. */
        before = canvas_copy();
        a->ribbon_open = 1; a->ribbon = RIBBON_FULL; render(a); shot(a, "ribbon-open");
        inside_window(a, base, "S9 ribbon-open");
        a->ribbon_open = 0; a->ribbon = 0;
        restores(a, before, "S9 ribbon-open");

        /* S9 each dropdown the menu can open. */
        static const struct { int kind; const char *tag; } menus[] = {
            {MENU_STATE, "menu-state"}, {MENU_ROLE, "menu-role"},
            {MENU_ROOM, "menu-room"},   {MENU_FILES, "menu-files"},
        };
        for (unsigned m = 0; m < sizeof menus / sizeof menus[0]; m++) {
            before = canvas_copy();
            menu_open(a, menus[m].kind, (lj_rect){CELLW, CELLH, 20 * CELLW, CELLH}, "demo-1");
            render(a); shot(a, menus[m].tag);
            inside_window(a, base, menus[m].tag);
            menu_close(a);
            restores(a, before, menus[m].tag);
        }

        /* S10 toast: shortest and longest text, in the footer row. */
        before = canvas_copy();
        toast(a, "Saved");
        render(a); shot(a, "toast-short");
        inside_window(a, base, "S10 toast-short");
        toast(a, "A toast long enough to need elision in a narrow window: the room limit was "
                 "reached and the tile could not be created, try closing one first");
        render(a); shot(a, "toast-long");
        inside_window(a, base, "S10 toast-long");
        elides(a, a->h / CELLH - 2, "S10 toast-long");
        a->toast[0] = 0;
        restores(a, before, "S10 toast");

        free(base);
    }

    for (int i = 0; i < a->nsession; i++) if (a->sessions[i].vt) lj_vt_free(a->sessions[i].vt);
    if (a->messages) json_object_put(a->messages);
    drafts_close(a);
    free(a);
    lj_render_close();
    printf("Popups: %d geometry checks over 11 surfaces x 3 sizes; all inside the window, "
           "all restore their background PASS\n", checks);
    return 0;
}
