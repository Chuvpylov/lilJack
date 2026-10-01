/* tab-icons-flicker — the icon-width contract.
 *
 * the operator live 2026-09-12: "tab/agent icons flicker, appear/disappear frame to
 * frame." The layout pass sizes every agent chip and room tab from
 * label_cells() (main.c), which measures the vendored table in c_render.c, and
 * the paint pass measures the SAME string with the system wcwidth() in
 * c_ansi.c:lj_ansi_text. Two width sources can disagree; when they do, a chip's
 * reserved width no longer matches what is painted, the next chip's panel
 * overwrites the icon's continuation cell, and erase_wide() blanks the glyph.
 *
 * This test pins the contract the fix relies on: every glyph in the emoji icon
 * set is ONE stable width, measured by the vendored table, and the assembled
 * identity string is stable across repeated calls. It is a planted test — it
 * FAILS against the table with the U+1F6E0..U+1F6EA gap (codex's hammer was
 * width 1 while brain/magnifier/herb were width 2) and passes once the icon
 * block is uniformly wide.
 */
#define main liljack_entry
#include "../liljack_app/main.c"
#undef main
#include "../liljack_app/c_ansi.c"
#include <assert.h>

int main(void){
    setlocale(LC_CTYPE,"C.UTF-8");
    const char *agents[]={"claude","codex","deepseek","other"};
    const char *names[]={"brain","hammer","magnifier","herb"};
    int ref=-1;
    for(int i=0;i<4;i++){
        const char *ic=icon(agents[i]);
        int c1=label_cells(ic),c2=label_cells(ic);
        assert(c1==c2);              /* stable across calls */
        assert(c1==2);               /* the declared icon width: two cells */
        if(ref<0)ref=c1;
        assert(c1==ref);             /* and uniform across the whole set */
        printf("icon %-8s (%-10s) cells=%d\n",agents[i],names[i],c1);
    }
    /* The assembled identity is what the chip actually measures; it must not
     * change between two calls with identical input. */
    for(int i=0;i<4;i++){
        char a[128],b[128];
        snprintf(a,sizeof a,"%s %s",icon(agents[i]),agents[i]);
        snprintf(b,sizeof b,"%s %s",icon(agents[i]),agents[i]);
        assert(label_cells(a)==label_cells(b));
        assert(label_cells(a)==label_cells(icon(agents[i]))+1+label_cells(agents[i]));
    }
    /* label_cells must agree with the codepoint width the table reports. */
    const char *ic=icon("codex");
    const char *q=ic;int sum=0;
    while(*q)sum+=lj_render_cells_for(nextcp(&q));
    assert(sum==label_cells(ic));
    puts("Icon width stable for the emoji set PASS");
        /* MEASURE vs ADVANCE (claude, 2026-09-12). The paint pass attaches a
     * zero-width codepoint to the previous cell (c_ansi.c:371) and consumes no
     * column; sizing must agree, or a label with a combining mark or variation
     * selector reserves more than it paints — the same disagreement class as
     * the icon width gap above. */
    {
        const char *plain = "codex", *marked = "codex\u0301";   /* + combining acute */
        int a = 0, b = 0;
        for (const char *q = plain; *q;) a += lj_render_cells_measure(nextcp(&q));
        for (const char *q = marked; *q;) b += lj_render_cells_measure(nextcp(&q));
        assert(a == 5 && b == 5);
        assert(lj_render_cells_measure(0xFE0F) == 0);      /* variation selector */
        assert(lj_render_cells_measure(0x0301) == 0);      /* combining acute */
        assert(lj_render_cells_measure(0x1F9E0) == 2 && lj_render_cells_measure('A') == 1);
        assert(lj_render_cells_for(0xFE0F) == 1);          /* advance still never 0 */
        printf("measure: zero-width costs no column (%d==%d), advance unchanged PASS\n", a, b);
    }
return 0;
}
