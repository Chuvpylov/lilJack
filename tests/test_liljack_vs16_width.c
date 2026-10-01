/* vs16-width — tile-header-doubled-glyphs-wezterm: the presenter must not
 * assume how far the host advances after an ambiguous-width icon (🛠 U+1F6E0).
 * With the host measured as NOT widening on VS16 (WezTerm: advance 1) the wire
 * carries the icon followed by a SPACE in its tail cell and no U+FE0F, so the
 * next glyph of the same run lands two cells on; with the host widening
 * (owkTerm: advance 2) the wire keeps U+FE0F. A partial redraw later in the
 * row must position explicitly (CUP) rather than trail the icon. */
#include "../liljack_app/c_ansi.c"
#include <assert.h>
#include <locale.h>
static int checks,failed;
#define CHECK(c,...) do{checks++;if(!(c)){failed++;fprintf(stderr,"FAIL %s:%d ",__FILE__,__LINE__);fprintf(stderr,__VA_ARGS__);fputc('\n',stderr);}}while(0)
static FILE *sink;
static size_t drain(char *buf,size_t cap){fflush(stdout);long end=ftell(sink);rewind(sink);size_t n=fread(buf,1,cap-1,sink);buf[n]=0;(void)end;rewind(sink);ftruncate(fileno(sink),0);return n;}
static int has(const char *hay,size_t n,const char *needle){size_t m=strlen(needle);for(size_t i=0;i+m<=n;i++)if(!memcmp(hay+i,needle,m))return 1;return 0;}
static void run(int wide){
    memset(&state,0,sizeof state);state.opened=1;state.sixel=0;state.cellw=10;state.cellh=20;state.vs16_wide=wide;
    lj_ansi_begin(800,560);
    lj_ansi_text(0,0,"\xf0\x9f\x9b\xa0 codex  /  mo-0",0xe1efff,400);   /* 🛠 codex  /  mo-0 */
    lj_ansi_text(50*LJ_CELL_W,0,"ROLE \xe2\x96\xbe",0xe1efff,100);
    assert(lj_ansi_present());
    static char wire[1<<16];size_t n=drain(wire,sizeof wire);
    int fe0f=has(wire,n,"\xef\xb8\x8f");
    int icon_space=has(wire,n,"\xf0\x9f\x9b\xa0 ");
    int icon_fe0f=has(wire,n,"\xf0\x9f\x9b\xa0\xef\xb8\x8f");
    if(wide){CHECK(icon_fe0f&&fe0f,"widening host: expected 🛠+FE0F on the wire");}
    else{CHECK(icon_space&&!fe0f,"non-widening host: expected '🛠 ' (icon + tail space) and no FE0F; fe0f=%d icon_space=%d",fe0f,icon_space);}
    /* partial redraw: only the ROLE cells change; the run must start with an explicit CUP at column 51 */
    lj_ansi_begin(800,560);
    lj_ansi_text(0,0,"\xf0\x9f\x9b\xa0 codex  /  mo-0",0xe1efff,400);
    lj_ansi_text(50*LJ_CELL_W,0,"ROLE \xe2\x96\xb4",0xe1efff,100);
    assert(lj_ansi_present());n=drain(wire,sizeof wire);
    CHECK(has(wire,n,"\033[1;56H")&&!has(wire,n,"\xf0\x9f\x9b\xa0"),"partial redraw: expected CUP to the changed cell only (no icon re-emitted); wire=%zu bytes",n);
    fprintf(stderr,"vs16 host=%s: wire %zu bytes, FE0F=%d, icon+space=%d\n",wide?"widens":"does not widen",n,fe0f,icon_space);
}
int main(void){
    setlocale(LC_CTYPE,"C.UTF-8");sink=tmpfile();assert(sink);assert(dup2(fileno(sink),1)>=0);
    run(0);run(1);
    /* the env override decides without a terminal */
    setenv("LJ_ANSI_VS16","0",1);state.vs16_wide=1;probe_vs16();CHECK(state.vs16_wide==0,"LJ_ANSI_VS16=0 not honoured");
    setenv("LJ_ANSI_VS16","1",1);probe_vs16();CHECK(state.vs16_wide==1,"LJ_ANSI_VS16=1 not honoured");unsetenv("LJ_ANSI_VS16");
    fprintf(stderr,"%s vs16-width: %d checks, %d failed (icon + tail space and no FE0F when the host does not widen; FE0F kept when it does; partial redraws position explicitly; env override)\n",failed?"FAIL":"PASS",checks,failed);
    return failed?1:0;
}
