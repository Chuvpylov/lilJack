/* theme-editor — THEME → Theme editor… opens a numeric settings dialog that
 * applies live (theme-editor-graph-settings). ANSI canvas, 3 sizes. Asserts:
 *   T1 the logo menu's Theme editor row is enabled and opens the dialog;
 *   T2 the dialog lists graph.bucket_ms and graph.repaint_ms with their values;
 *   T3 [+] on the bucket row raises lj_metrics_display_ms by one step, [-] lowers it,
 *      the bounds hold (25..1000), and the repaint never drops below the bucket;
 *   T4 keyboard: Down selects the repaint row, Right steps it, Esc closes;
 *   T5 closing restores the canvas under the dialog byte for byte.
 * Fail-before: the row is disabled ("soon") and no dialog exists. */
#define main liljack_entry
#include "../liljack_app/main.c"
#undef main
#include "../liljack_app/c_ansi.c"
#include <assert.h>
#include <locale.h>
#include <unistd.h>
static int checks,failed;
#define CHECK(c,...) do{checks++;if(!(c)){failed++;fprintf(stderr,"FAIL %s:%d ",__FILE__,__LINE__);fprintf(stderr,__VA_ARGS__);fputc('\n',stderr);}}while(0)
static const cell *at(int col,int row){return &state.canvas[row*state.cols+col];}
static void row_text(int row,char *out,size_t cap){size_t n=0;for(int c=0;c<state.cols&&n+4<cap;c++){uint32_t cp=at(c,row)->cp;if(!cp)out[n++]=' ';else if(cp<128)out[n++]=(char)cp;else n+=(size_t)snprintf(out+n,cap-n,"<%X>",cp);}out[n]=0;}
static int find_row(const char *needle){for(int r=0;r<state.rows;r++){char t[2048];row_text(r,t,sizeof t);if(strstr(t,needle))return r;}return -1;}
static Hit *find_hit(App *a,int kind,int index){for(int i=a->nhits-1;i>=0;i--)if(a->hits[i].kind==kind&&(index<0||a->hits[i].index==index))return &a->hits[i];return NULL;}
static void press(App *a,SDL_Keycode k){SDL_Event e={0};e.type=SDL_KEYDOWN;e.key.keysym.sym=k;event(a,&e);}
static void typed(App *a,const char *t){SDL_Event e={0};e.type=SDL_TEXTINPUT;snprintf(e.text.text,sizeof e.text.text,"%s",t);event(a,&e);}
static void frame(App *a){lj_ansi_begin(a->w,a->h);render(a);}
int main(void){
    setlocale(LC_CTYPE,"C.UTF-8");assert(!lj_render_init(1920,1080));lj_metrics_init();
    int sizes[3][2]={{800,560},{1280,720},{1920,1080}};
    for(int s=0;s<3;s++){
        App *a=calloc(1,sizeof *a);assert(a);a->w=sizes[s][0];a->h=sizes[s][1];a->ansi=1;a->demo=1;a->split=-1;a->backend_in=a->backend_out=-1;
        for(int i=0;i<COUNT;i++)a->sessions[i].fd=-1;
        lj_render_resize(a->w,a->h);lj_dock_init(&a->dock);lj_media_init(&a->media);lj_popup_init(&a->popup);demo(a);
        frame(a);size_t bytes=(size_t)state.cols*state.rows*sizeof(cell);cell *before=malloc(bytes);memcpy(before,state.canvas,bytes);char toast0[sizeof a->toast];copy(toast0,sizeof toast0,a->toast);
        /* T1 */
        Hit *b=find_hit(a,H_BURGER,-1);assert(b);Hit burger=*b;activate(a,&burger);frame(a);
        MenuRow rows[24];int n=menu_rows(a,rows,24),ti=-1;for(int i=0;i<n;i++)if(strstr(rows[i].label,"Theme editor"))ti=i;
        CHECK(ti>=0&&!rows[ti].disabled,"%dx%d Theme editor row index %d disabled=%d",a->w,a->h,ti,ti>=0?rows[ti].disabled:-1);
        Hit *th=ti>=0?find_hit(a,H_MENU_ITEM,ti):NULL;CHECK(th!=NULL,"%dx%d no Theme editor hit",a->w,a->h);
        if(th){Hit t=*th;activate(a,&t);}frame(a);
        int title=find_row("THEME EDITOR");CHECK(title>=0,"%dx%d dialog title missing",a->w,a->h);
        /* T2 */
        int r1=find_row("graph.bucket_ms"),r2=find_row("graph.repaint_ms");
        CHECK(r1>0&&r2>r1,"%dx%d rows bucket=%d repaint=%d",a->w,a->h,r1,r2);
        {char t[2048];if(r1>0){row_text(r1,t,sizeof t);char v[32];snprintf(v,sizeof v,"%5d ms",lj_metrics_display_ms());CHECK(strstr(t,v)!=NULL,"%dx%d bucket row '%s' lacks '%s'",a->w,a->h,t,v);}}
        /* T3 */
        /* the unified row model: header SETTINGS, then the numeric rows */
        ThemeRow trows[THEME_ROWS_MAX];int tn=theme_rows(trows,THEME_ROWS_MAX),rb=-1,rr=-1,rc=-1,hdrs=0;
        for(int i=0;i<tn;i++){if(trows[i].kind==TR_SETTING&&trows[i].index==0)rb=i;if(trows[i].kind==TR_SETTING&&trows[i].index==1)rr=i;if(trows[i].kind==TR_COLOUR&&rc<0)rc=i;hdrs+=trows[i].kind==TR_HEADER;}
        CHECK(rb==1&&rr==2&&hdrs==3&&rc>rr,"%dx%d row model: bucket %d repaint %d first colour %d headers %d of %d rows",a->w,a->h,rb,rr,rc,hdrs,tn);
        CHECK(find_row("SETTINGS")>=0&&find_row("COLOURS")>=0,"%dx%d section headers not shown",a->w,a->h);
        int base=lj_metrics_display_ms();Hit *inc=find_hit(a,H_SET_INC,rb),*dec=find_hit(a,H_SET_DEC,rb);CHECK(inc&&dec,"%dx%d +/- hits",a->w,a->h);
        if(!inc||!dec){fprintf(stderr,"FAIL theme-editor: aborted, no +/- hits\n");return 1;}
        if(inc){Hit h=*inc;activate(a,&h);CHECK(lj_metrics_display_ms()==base+25,"%dx%d + gave %d",a->w,a->h,lj_metrics_display_ms());}
        if(dec){Hit h=*dec;activate(a,&h);CHECK(lj_metrics_display_ms()==base,"%dx%d - gave %d",a->w,a->h,lj_metrics_display_ms());}
        for(int i=0;i<60;i++){Hit h=*dec;activate(a,&h);}CHECK(lj_metrics_display_ms()==25,"%dx%d lower bound %d",a->w,a->h,lj_metrics_display_ms());
        for(int i=0;i<60;i++){Hit h=*inc;activate(a,&h);}CHECK(lj_metrics_display_ms()==1000&&lj_metrics_repaint_ms()>=1000,"%dx%d upper bound %d repaint %d",a->w,a->h,lj_metrics_display_ms(),lj_metrics_repaint_ms());
        {Hit *rs=find_hit(a,H_SET_RESET,-1);CHECK(rs!=NULL,"reset hit");if(rs){Hit h=*rs;activate(a,&h);}CHECK(lj_metrics_display_ms()==50&&lj_metrics_repaint_ms()==100,"%dx%d reset %d/%d",a->w,a->h,lj_metrics_display_ms(),lj_metrics_repaint_ms());}
        /* T3b the theme follows the edits and SAVE persists them */
        {Hit h=*inc;activate(a,&h);CHECK(lj_theme_int(LJ_THEME_GRAPH_BUCKET_MS)==75&&lj_metrics_display_ms()==75,"%dx%d theme/metrics after + : %d/%d",a->w,a->h,lj_theme_int(LJ_THEME_GRAPH_BUCKET_MS),lj_metrics_display_ms());
         frame(a);Hit *sv=find_hit(a,H_SET_SAVE,-1);CHECK(sv!=NULL,"save hit");
         char path[512];snprintf(path,sizeof path,"%s/theme-editor-%d.json",getenv("TMPDIR")?getenv("TMPDIR"):"/tmp",a->w);setenv("LILJACK_THEME",path,1);unlink(path);
         if(sv){Hit h2=*sv;activate(a,&h2);}
         FILE *f=fopen(path,"r");CHECK(f!=NULL,"%dx%d SAVE wrote nothing at %s (toast '%s')",a->w,a->h,path,a->toast);
         if(f){char buf[65536];size_t n=fread(buf,1,sizeof buf-1,f);buf[n]=0;fclose(f);CHECK(strstr(buf,"graph.bucket_ms")&&strstr(buf,"75"),"%dx%d saved theme lacks graph.bucket_ms 75",a->w,a->h);}
         /* reload from the saved file: the value comes back and reaches the plots without env */
         {Hit h3=*dec;activate(a,&h3);}CHECK(lj_metrics_display_ms()==50,"pre-reload %d",lj_metrics_display_ms());
         char err[256];CHECK(lj_theme_load_default(err,sizeof err)==1,"%dx%d reload: %s",a->w,a->h,err);unsetenv("LJ_METRICS_DISPLAY_MS");theme_apply_metrics(1);
         CHECK(lj_metrics_display_ms()==75,"%dx%d after reload+startup hookup bucket=%d (want 75)",a->w,a->h,lj_metrics_display_ms());
         /* env precedence at init: a valid env keeps its value */
         setenv("LJ_METRICS_DISPLAY_MS","25",1);lj_metrics_close();lj_metrics_init();theme_apply_metrics(1);
         CHECK(lj_metrics_display_ms()==25,"%dx%d env 25 lost at init: %d",a->w,a->h,lj_metrics_display_ms());
         unsetenv("LJ_METRICS_DISPLAY_MS");lj_metrics_close();lj_metrics_init();theme_apply_metrics(1);CHECK(lj_metrics_display_ms()==75,"no-env restore %d",lj_metrics_display_ms());
         /* env contract per key (lead 1576): INTERVAL_MS 0 (never limit) and 5000 are valid overrides and survive the startup hookup; unset → the theme's 25 */
         setenv("LJ_METRICS_INTERVAL_MS","0",1);lj_metrics_close();lj_metrics_init();theme_apply_all(a,1);CHECK(lj_metrics_interval_ms()==0,"%dx%d env interval 0 lost: %d",a->w,a->h,lj_metrics_interval_ms());
         setenv("LJ_METRICS_INTERVAL_MS","5000",1);lj_metrics_close();lj_metrics_init();theme_apply_all(a,1);CHECK(lj_metrics_interval_ms()==5000,"%dx%d env interval 5000 lost: %d",a->w,a->h,lj_metrics_interval_ms());
         unsetenv("LJ_METRICS_INTERVAL_MS");lj_metrics_close();lj_metrics_init();theme_apply_all(a,1);CHECK(lj_metrics_interval_ms()==lj_theme_int(LJ_THEME_METRICS_POLL_MS),"%dx%d no env: interval %d, theme %d",a->w,a->h,lj_metrics_interval_ms(),lj_theme_int(LJ_THEME_METRICS_POLL_MS));
         unlink(path);unsetenv("LILJACK_THEME");lj_theme_set_int(LJ_THEME_GRAPH_BUCKET_MS,50);lj_theme_set_int(LJ_THEME_GRAPH_REPAINT_MS,100);theme_apply_metrics(0);}
        /* T4 */
        a->settings_sel=rb;press(a,SDLK_DOWN);CHECK(a->settings_sel==rr,"%dx%d Down sel %d (want the repaint row %d)",a->w,a->h,a->settings_sel,rr);
        int rp=lj_metrics_repaint_ms(),rstep=lj_theme_setting_get(LJ_THEME_GRAPH_REPAINT_MS)->step;press(a,SDLK_RIGHT);CHECK(lj_metrics_repaint_ms()==rp+rstep,"%dx%d Right repaint %d (step %d)",a->w,a->h,lj_metrics_repaint_ms(),rstep);
        press(a,SDLK_LEFT);
        /* T4b colours + scrolling: the first colour row shows its live hex; PgDn scrolls; GLYPHS reachable */
        a->settings_sel=rc;frame(a);{char want[16];snprintf(want,sizeof want,"#%06x",lj_theme_rgb((lj_theme_id)trows[rc].index)&0xffffff);CHECK(find_row(want)>=0,"%dx%d colour row lacks %s",a->w,a->h,want);}
        {int before_scroll=a->settings_scroll;for(int i=0;i<30;i++)press(a,SDLK_PAGEDOWN);frame(a);CHECK(a->settings_scroll>before_scroll&&find_row("U+")>=0,"%dx%d PgDn did not reach the glyph rows (scroll %d)",a->w,a->h,a->settings_scroll);}
        /* colour edit: typed hex applies through lj_theme_set_rgb (codex), per-channel step too;
         * the text must never reach the focused tile while the editor is open */
        copy(a->focus,sizeof a->focus,"demo-0");{Session *s0=session(a,"demo-0");size_t q0=s0?s0->queued:0;typed(a,"ab");Session *s1=session(a,"demo-0");CHECK(!s1||s1->queued==q0,"%dx%d typed text leaked to the focused tile",a->w,a->h);a->settings_hex[0]=0;}copy(a->focus,sizeof a->focus,"room");
        {a->settings_sel=rc;uint32_t was=lj_theme_rgb((lj_theme_id)trows[rc].index);typed(a,"12");typed(a,"34");typed(a,"5");typed(a,"6");press(a,SDLK_RETURN);
         CHECK((lj_theme_rgb((lj_theme_id)trows[rc].index)&0xffffff)==0x123456,"%dx%d typed hex not applied: %06x",a->w,a->h,lj_theme_rgb((lj_theme_id)trows[rc].index)&0xffffff);
         a->settings_chan=0;press(a,SDLK_RIGHT);CHECK((lj_theme_rgb((lj_theme_id)trows[rc].index)&0xffffff)==0x1a3456,"%dx%d R+8 gave %06x",a->w,a->h,lj_theme_rgb((lj_theme_id)trows[rc].index)&0xffffff);
         lj_theme_set_rgb((lj_theme_id)trows[rc].index,was);}
        a->settings_sel=rb;press(a,SDLK_ESCAPE);CHECK(!a->settings_open,"%dx%d Esc left dialog open",a->w,a->h);
        /* T5 (the SAVE toast lives in the footer, not in the dialog: put the original toast back first) */
        copy(a->toast,sizeof a->toast,toast0);frame(a);{int diff=-1;for(int i=state.cols;i<state.cols*state.rows;i++)if(memcmp(&before[i],&state.canvas[i],sizeof(cell))){diff=i;break;}
         if(diff>=0)fprintf(stderr,"  first diff cell col %d row %d: before cp %04X bg %06x / after cp %04X bg %06x\n",diff%state.cols,diff/state.cols,before[diff].cp,before[diff].bg&0xffffff,state.canvas[diff].cp,state.canvas[diff].bg&0xffffff);
         CHECK(diff<0,"%dx%d canvas not restored (rows 1..end; row 0 holds the live plots)",a->w,a->h);}
        free(before);for(int i=0;i<a->nsession;i++)if(a->sessions[i].vt)lj_vt_free(a->sessions[i].vt);if(a->messages)json_object_put(a->messages);drafts_close(a);free(a);
    }
    lj_metrics_close();lj_render_close();
    fprintf(stderr,"%s theme-editor: %d checks, %d failed (menu entry enabled, dialog rows + values, +/- live apply with bounds, reset, keyboard Down/Right/Esc, canvas restored) at 3 sizes\n",failed?"FAIL":"PASS",checks,failed);
    return failed?1:0;
}
