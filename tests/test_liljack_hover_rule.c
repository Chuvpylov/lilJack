/* hover-rule — visual-polish-chrome item 2: ONE hover rule for every clickable
 * control (the operator: "hover over tabs that persist in all UI"). On the ANSI canvas,
 * with the pointer over a control, its first cell carries the lifted surface
 * (SURF3) or the active surface (GREEN). Bodies, drag handles, scrims, text
 * fields, sliders and list rows are not buttons and are excluded by kind.
 * Surfaces exercised: the demo workspace, the logo menu, the media popup
 * (synthetic frame) and the theme editor dialog, at 800/1280/1920. */
#define main liljack_entry
#include "../liljack_app/main.c"
#undef main
#include "../liljack_app/c_ansi.c"
#include <assert.h>
#include <locale.h>
static int checks,failed;
#define CHECK(c,...) do{checks++;if(!(c)){failed++;fprintf(stderr,"FAIL %s:%d ",__FILE__,__LINE__);fprintf(stderr,__VA_ARGS__);fputc('\n',stderr);}}while(0)
static const cell *at(int col,int row){return &state.canvas[row*state.cols+col];}
static void frame(App *a){lj_ansi_begin(a->w,a->h);render(a);}
static uint32_t frame_px[640*360];
static int excluded(int k){
    switch(k){case H_HEADER:case H_TERMINAL:case H_REVIEW_BODY:case H_DASH_BODY:case H_GIT_BODY:case H_FILES_BODY:
    case H_ROOM_FIELD:case H_MENU_SCRIM:case H_POPUP_BODY:case H_POPUP_DRAG:case H_POPUP_SEEK:case H_POPUP_VOLUME:
    case H_POPUP_RESIZE:case H_SET_ROW:case H_ROOM:case H_STAGE:case H_DEST:case H_ANSWER:case H_ANSWER_DM:case H_ROOM_TAB:
    case H_FILE_ENTRY:case H_ROOM_FOLDER_ENTRY:case H_ROOM_AGENT:case H_MENU_ITEM:case H_RIBBON_ITEM:case H_FILE_MENU:return 1;default:return 0;}
}
static int lifted(uint32_t bg){bg&=0xffffff;return bg==(SURF3&0xffffff)||bg==(GREEN&0xffffff);}
static void sweep(App *a,const char *surface){
    frame(a);int n=a->nhits;Hit hits[512];memcpy(hits,a->hits,sizeof(Hit)*(size_t)n);
    int seen[256]={0},bad[256]={0},tested=0;
    for(int i=0;i<n;i++){Hit *h=&hits[i];if(excluded(h->kind)||h->kind<=0||h->kind>=256||h->r.w<CELLW||h->r.h<CELLH)continue;
        if(seen[h->kind]++)continue;                    /* one probe per kind per surface */
        a->mousex=h->r.x+CELLW/2;a->mousey=h->r.y+CELLH/2;frame(a);frame(a);tested++;
        /* the pointer may now resolve to a raised overlay covering this control; then skip */
        if(a->hover_kind!=h->kind)continue;
        /* skip a control whose probe cell lies under a higher-layer surface
         * (an open menu or the popup paints over it — occlusion, not a rule breach) */
        {int covered=0;for(int j=0;j<a->nhits;j++){Hit *o=&a->hits[j];if(o->layer>h->layer&&inside(o->r,a->mousex,a->mousey)){covered=1;break;}}if(covered)continue;}
        int col=h->r.x/CELLW,row=h->r.y/CELLH;uint32_t bg=at(col,row)->bg;
        if(!lifted(bg)){bad[h->kind]=1;fprintf(stderr,"  %s %dx%d kind %d id '%s' at cell %d,%d: bg %06x not lifted\n",surface,a->w,a->h,h->kind,h->id,col,row,bg&0xffffff);}
    }
    int nbad=0;for(int k=0;k<256;k++)nbad+=bad[k];
    CHECK(nbad==0,"%s %dx%d: %d control kinds ignore the hover rule (%d probed)",surface,a->w,a->h,nbad,tested);
    a->mousex=a->mousey=-1;
}
int main(void){
    setlocale(LC_CTYPE,"C.UTF-8");assert(!lj_render_init(1920,1080));lj_metrics_init();
    for(int i=0;i<640*360;i++)frame_px[i]=0xff203050u+(uint32_t)(i%97);
    int sizes[3][2]={{800,560},{1280,720},{1920,1080}};
    for(int s=0;s<3;s++){
        App *a=calloc(1,sizeof *a);assert(a);a->w=sizes[s][0];a->h=sizes[s][1];a->ansi=1;a->demo=1;a->split=-1;a->backend_in=a->backend_out=-1;
        for(int i=0;i<COUNT;i++)a->sessions[i].fd=-1;a->mousex=a->mousey=-1;
        lj_render_resize(a->w,a->h);lj_dock_init(&a->dock);lj_media_init(&a->media);lj_popup_init(&a->popup);demo(a);
        sweep(a,"workspace");
        frame(a);Hit *b=NULL;for(int i=a->nhits-1;i>=0;i--)if(a->hits[i].kind==H_BURGER){b=&a->hits[i];break;}assert(b);Hit burger=*b;
        activate(a,&burger);sweep(a,"logo-menu");menu_close(a);
        lj_popup *p=&a->popup;p->open=1;p->media.pixels=frame_px;p->media.w=640;p->media.h=360;p->media.is_video=1;p->media.duration=212;p->media.clock_base=42;p->media.has_audio=1;p->media.volume=70;a->popup_placed=0;
        sweep(a,"popup");p->media.pixels=NULL;p->open=0;a->popup_placed=0;
        a->settings_open=1;a->settings_sel=0;sweep(a,"theme-editor");a->settings_open=0;
        a->about_open=1;sweep(a,"about");a->about_open=0;
        for(int i=0;i<a->nsession;i++)if(a->sessions[i].vt)lj_vt_free(a->sessions[i].vt);if(a->messages)json_object_put(a->messages);drafts_close(a);free(a);
    }
    lj_metrics_close();lj_render_close();
    fprintf(stderr,"%s hover-rule: %d checks, %d failed (every probed control kind lifts to SURF3/GREEN under the pointer: workspace, logo menu, popup, theme editor, about) at 3 sizes\n",failed?"FAIL":"PASS",checks,failed);
    return failed?1:0;
}
