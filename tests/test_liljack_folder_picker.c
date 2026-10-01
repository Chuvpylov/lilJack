/* room-dialog-folder-browser — every folder browser control shows its whole label
 * and sits inside the window, at the default size and at the minimum size.
 *
 * The inline four-row picker (UP/PREV/NEXT/USE FOLDER, 2026-09-12) became a
 * floating window on 2026-09-16 (the operator: "make it a proper popup floating
 * window"). This keeps the original MEASUREMENT rule: a control is clipped when
 * its label is wider than what button() leaves after padding on BOTH sides.
 * It also checks that no control spills out of the window, including when the
 * window has been shrunk to FB_MIN_COLS x FB_MIN_ROWS.
 */
#define main liljack_entry
#include "../liljack_app/main.c"
#undef main
#include "../liljack_app/c_ansi.c"
#include <assert.h>
#include <locale.h>

static int checks;

/* What button() actually leaves for the text, in pixels. */
static int label_room(lj_rect r) { int pad = r.w < 40 ? 5 : 9; return r.w - pad * 2; }

static void fits(const char *label, lj_rect box, const char *what, int w, int h) {
    int cells = 0;
    for (const char *q = label; *q;) cells += lj_render_cells_measure(nextcp(&q));
    int need = cells * CELLW, have = label_room(box);
    printf("  %-12s %4dx%-4d label=%-12s needs %3dpx, box leaves %3dpx\n", what, w, h, label, need, have);
    assert(need <= have);
    checks++;
}
static void within(lj_rect in, lj_rect out, const char *what) {
    if (!(in.x >= out.x && in.y >= out.y && in.x + in.w <= out.x + out.w && in.y + in.h <= out.y + out.h)) {
        fprintf(stderr, "%s spills out of the browser: %d,%d %dx%d vs %d,%d %dx%d\n", what, in.x, in.y, in.w, in.h, out.x, out.y, out.w, out.h);
        abort();
    }
    checks++;
}

int main(void) {
    setlocale(LC_CTYPE, "C.UTF-8");
    assert(!lj_render_init(800, 560));
    int sizes[][2] = {{800, 560}, {1280, 720}, {1920, 1080}};
    for (unsigned z = 0; z < sizeof sizes / sizeof sizes[0]; z++) {
        App *a = calloc(1, sizeof *a);
        assert(a);
        a->w = sizes[z][0]; a->h = sizes[z][1];
        a->demo = a->ansi = 1; a->split = -1; a->backend_in = a->backend_out = -1;
        for (int i = 0; i < COUNT; i++) a->sessions[i].fd = -1;
        lj_dock_init(&a->dock); lj_media_init(&a->media); lj_popup_init(&a->popup);
        demo(a);
        a->mousex = a->mousey = -1;
        /* A real, populated folder: the repo itself always has subdirectories. */
        copy(a->project, sizeof a->project, ".");
        room_dialog_open(a);
        render(a);
        for (int i = 0; i < a->nhits; i++)
            if (a->hits[i].kind == H_ROOM_FOLDER_BROWSE) fits(FB_FOLDER_ICON " BROWSE", a->hits[i].r, "BROWSE", a->w, a->h);
        folder_browser_open(a);
        for (int pass = 0; pass < 2; pass++) {
            if (pass) { a->fb_rect.w = FB_MIN_COLS * CELLW; a->fb_rect.h = FB_MIN_ROWS * CELLH; }
            render(a);
            lj_rect win = a->fb_rect;
            printf("  window       %4dx%-4d %s %dx%d cells\n", a->w, a->h, pass ? "minimum" : "default", win.w / CELLW, win.h / CELLH);
            int seen = 0;
            for (int i = 0; i < a->nhits; i++) {
                Hit *hh = &a->hits[i];
                const char *label = NULL;
                switch (hh->kind) {
                case H_ROOM_FOLDER_UP: label = "▴ UP"; break;
                case H_FB_HOME: label = "⌂ HOME"; break;
                case H_FB_PROJECT: label = "PROJECT"; break;
                case H_FB_HIDDEN: label = a->fb_hidden ? "● HIDDEN" : "○ HIDDEN"; break;
                case H_FB_CANCEL: label = "CANCEL"; break;
                case H_ROOM_FOLDER_USE: label = "USE FOLDER"; break;
                default: break;
                }
                if (label) { fits(label, hh->r, label, a->w, a->h); within(hh->r, win, label); seen++; }
                if (hh->kind == H_ROOM_FOLDER_ENTRY || hh->kind == H_FB_OPEN || hh->kind == H_FB_CRUMB || hh->kind == H_FB_PATH || hh->kind == H_FB_CLOSE)
                    within(hh->r, win, "row/crumb/path/close");
            }
            assert(seen == 6);   /* UP HOME PROJECT HIDDEN CANCEL USE, at both sizes */
            /* Populated folder coverage: the list shows real entries. */
            if (a->room_folder_count > 0) {
                int shown = 0;
                for (int i = 0; i < a->nhits; i++) if (a->hits[i].kind == H_ROOM_FOLDER_ENTRY) shown++;
                assert(shown > 0 && shown <= a->fb_rows);
                printf("  entry rows   %4dx%-4d %d of %d listed\n", a->w, a->h, shown, a->room_folder_count);
                checks++;
            }
        }
        a->room_dialog = 0;
        for (int i = 0; i < a->nsession; i++) if (a->sessions[i].vt) lj_vt_free(a->sessions[i].vt);
        if (a->messages) json_object_put(a->messages);
        drafts_close(a);
        free(a);
    }
    lj_render_close();
    printf("Folder picker: %d checks — every browser control fits its label and stays inside the window (default and minimum), entries listed PASS\n", checks);
    return 0;
}
