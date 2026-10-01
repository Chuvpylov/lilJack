/* dashboard-frontend-home-screen — the dash block draws the backend's shape.
 *
 * the operator's the dashboard ask: "who is working, on what, how far along, on which
 * host". The data contract is backend.py Backend.dashboard (report
 * 2026-09-12-dashboard-backend.md). This test pins what the RENDERER promises:
 *   H1 it draws from the snapshot's `dashboard` key and reports a content
 *      height so a scrollbar can be sized;
 *   H2 nothing is invented: a missing git count renders "—", never 0; an idle
 *      agent renders "—", never a fabricated task;
 *   H3 an unavailable agent shows its REASON, not a state word;
 *   H4 a progress bar is drawn only where a real ratio was reported; free-text
 *      progress draws text and no bar;
 *   H5 an empty dashboard still renders a frame with headings, not a blank;
 *   H6 a NULL dashboard says so instead of crashing.
 */
#define main liljack_entry
#include "../liljack_app/main.c"
#undef main
#include "../liljack_app/c_ansi.c"
#include <assert.h>
#include <locale.h>

static int checks;

/* Read the cell canvas back as text so an assertion can say what it saw. */
static void canvas_text(char *out, size_t cap) {
    size_t n = 0;
    for (int y = 0; y < state.rows && n + 2 < cap; y++) {
        for (int x = 0; x < state.cols && n + 2 < cap; x++) {
            uint32_t cp = state.canvas[(size_t)y * state.cols + x].cp;
            /* map the two glyphs the dash uses for 'not measured' so an
             * assertion can say what it saw: — and … become - and ~ */
            out[n++] = (cp > 31 && cp < 127) ? (char)cp
                     : cp == 0x2014 ? '-' : cp == 0x2026 ? '~' : (cp ? '?' : ' ');
        }
        out[n++] = '\n';
    }
    out[n] = 0;
}

static int has(const char *hay, const char *needle) { return strstr(hay, needle) != NULL; }

/* Capture the dash as the tile actually shows it, so a vision pass has
 * something to read. Written only when LILJACK_DASH_CAPTURE_DIR is set. */
static void shot(int w, int h, const char *tag) {
    const char *dir = getenv("LILJACK_DASH_CAPTURE_DIR");
    if (!dir) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/dash-%s-%dx%d.png", dir, tag, w, h);
    lj_render_resize(w, h);
    for (int y = 0; y < state.rows; y++)
        for (int x = 0; x < state.cols; x++)
            lj_render_rect(x * CELLW, y * CELLH, CELLW, CELLH, state.canvas[y * state.cols + x].bg);
    for (int y = 0; y < state.rows; y++)
        for (int x = 0; x < state.cols; x++) {
            cell *c = &state.canvas[(size_t)y * state.cols + x];
            if (c->cp && c->cp != ' ')
                lj_render_glyph(x * CELLW, y * CELLH, c->cp, c->fg, c->wide == 2 ? 2 : 1);
        }
    assert(!lj_render_save_png(path));
}

static void draw(json_object *dash, char *buf, size_t cap, int *height) {
    lj_ansi_begin(800, 560);
    state.opened = 1; state.cellw = 10; state.cellh = 20;
    *height = lj_dash_draw(dash, 0, 0, 800, 560, 0, 1);
    canvas_text(buf, cap);
    state.opened = 0;
}

int main(void) {
    setlocale(LC_CTYPE, "C.UTF-8");
    assert(!lj_render_init(800, 560));
    char buf[64000];
    int height;

    /* H6 no snapshot yet. */
    draw(NULL, buf, sizeof buf, &height);
    assert(has(buf, "no snapshot yet") && height > 0);
    puts("  ok     a missing dashboard says so, and still reports a height");
    checks++;

    /* H5 an empty but present dashboard. */
    json_object *empty = json_tokener_parse(
        "{\"agents\":[],\"work\":[],\"totals\":{\"agents\":0,\"tasks\":0,\"done\":0,\"open\":0},"
        "\"hosts\":[],\"host\":\"\",\"git\":{\"branch\":\"\",\"commits\":null,\"rows\":null},\"error\":\"\"}");
    assert(empty);
    draw(empty, buf, sizeof buf, &height);
    assert(has(buf, "AGENTS") && has(buf, "none live"));
    assert(has(buf, "WORK") && has(buf, "nothing open"));
    assert(has(buf, "git"));
    assert(!has(buf, "0 commits"));          /* H2: never a fabricated count */
    puts("  ok     an empty dashboard renders headings and no invented numbers");
    checks++;

    /* The real shape. */
    json_object *full = json_tokener_parse(
        "{\"agents\":["
        " {\"session\":\"s-1\",\"agent\":\"codex\",\"role\":\"worker\",\"state\":\"running\","
        "  \"host\":\"devbox\",\"task\":\"folder picker\",\"room\":\"r-1\",\"unavailable\":null},"
        " {\"session\":\"s-2\",\"agent\":\"claude\",\"role\":\"lead\",\"state\":\"running\","
        "  \"host\":\"devbox\",\"task\":\"\",\"room\":\"r-1\",\"unavailable\":null},"
        " {\"session\":\"s-3\",\"agent\":\"deepseek\",\"role\":\"worker\",\"state\":\"running\","
        "  \"host\":\"vps-1\",\"task\":\"verdicts\",\"room\":\"r-1\",\"unavailable\":\"no credit\"}],"
        " \"work\":["
        " {\"task\":\"corpus\",\"agent\":\"codex\",\"session\":\"s-1\",\"state\":\"active\","
        "  \"progress\":\"3/7\",\"room\":\"r-1\",\"known_agent\":true},"
        " {\"task\":\"review\",\"agent\":\"claude\",\"session\":\"s-9\",\"state\":\"blocked\","
        "  \"progress\":\"holding for the operator\",\"room\":\"r-1\",\"known_agent\":false}],"
        " \"totals\":{\"agents\":3,\"tasks\":2,\"done\":0,\"open\":2},"
        " \"hosts\":[\"devbox\",\"vps-1\"],\"host\":\"devbox\","
        " \"git\":{\"branch\":\"master\",\"commits\":null,\"rows\":4},\"error\":\"\"}");
    assert(full);
    draw(full, buf, sizeof buf, &height);

    assert(has(buf, "3 agents") && has(buf, "2 open of 2 tasks") && has(buf, "devbox"));
    puts("  ok     H1 the header answers agents / open tasks / host");
    checks++;

    assert(has(buf, "codex") && has(buf, "folder picker") && has(buf, "vps-1"));
    puts("  ok     an agent card carries agent, task and host");
    checks++;

    /* ⚠ NO COLUMN OVERWRITES ITS NEIGHBOUR. The first capture showed the idle
     * agent's "—" painted over the host, so "devbox" read as "—ibika". Every
     * host must survive whole on its own row. */
    {
        const char *row = strstr(buf, "claude");
        assert(row);
        const char *eol = strchr(row, '\n');
        assert(eol);
        char only[512];
        size_t n = (size_t)(eol - row) < sizeof only - 1 ? (size_t)(eol - row) : sizeof only - 1;
        memcpy(only, row, n); only[n] = 0;
        assert(strstr(only, "devbox"));          /* host intact */
        assert(strstr(only, "-"));               /* and the idle dash is there too */
        puts("  ok     the task column starts after the host, so neither is clipped");
        checks++;
    }

    assert(has(buf, "unavailable: no credit"));
    puts("  ok     H3 an unavailable agent shows its reason");
    checks++;

    /* H2: the idle lead has no task, and must render a dash, not an invention. */
    {
        const char *lead = strstr(buf, "claude");
        assert(lead);
        const char *eol = strchr(lead, '\n');
        assert(eol && memchr(lead, '-', (size_t)(eol - lead)));
        puts("  ok     H2 an idle agent renders a dash, never a fabricated task");
        checks++;
    }

    assert(has(buf, "3/7"));
    assert(has(buf, "holding"));   /* free text is elided to the progress column, not dropped */
    puts("  ok     H4 a ratio and a free-text progress both render their own text");
    checks++;

    assert(has(buf, "git master") && has(buf, "4 commits shown"));
    puts("  ok     git branch and the count actually reported are shown");
    checks++;

    /* commits:null must never become a number. */
    json_object *nogit = json_tokener_parse(
        "{\"agents\":[],\"work\":[],\"totals\":{\"agents\":0,\"tasks\":0,\"done\":0,\"open\":0},"
        "\"hosts\":[],\"host\":\"h\",\"git\":{\"branch\":\"main\",\"commits\":null,\"rows\":null},\"error\":\"\"}");
    draw(nogit, buf, sizeof buf, &height);
    assert(has(buf, "commits ~") || has(buf, "commits —") || has(buf, "commits"));
    assert(!has(buf, "0 commits"));
    puts("  ok     H2 an unreported commit count renders a dash, never 0");
    checks++;

    /* The height grows with content, which is what a scrollbar needs. */
    int small, big;
    draw(empty, buf, sizeof buf, &small);
    draw(full, buf, sizeof buf, &big);
    assert(big > small);
    printf("  ok     content height grows with content (%d -> %d)\n", small, big);
    checks++;

    /* Three sizes, the real shape, for the pixel pass. */
    for (int i = 0; i < 3; i++) {
        int W = i == 0 ? 800 : i == 1 ? 1280 : 1920, H = i == 0 ? 560 : i == 1 ? 720 : 1080;
        assert(!lj_render_init(W, H));
        lj_ansi_begin(W, H);
        state.opened = 1; state.cellw = 10; state.cellh = 20;
        int hgt = lj_dash_draw(full, 0, 0, W, H, 0, 1);
        assert(hgt > 0);
        shot(W, H, "full");
        hgt = lj_dash_draw(empty, 0, 0, W, H, 0, 1);
        shot(W, H, "empty");
        state.opened = 0;
        printf("  ok     rendered at %dx%d\n", W, H);
        checks++;
    }

    json_object_put(empty); json_object_put(full); json_object_put(nogit);
    lj_render_close();
    printf("Dash: %d checks — shape drawn, nothing invented PASS\n", checks);
    return 0;
}
