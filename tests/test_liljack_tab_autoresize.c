/* tab-autoresize — the operator 2026-09-16 (room "lilJack's debug"): "this one liner is too
 * large and takes sooo much space in its tab"; "stylise tabs in sixel, better
 * alignment, autoresize". ANSI canvas (the cells the sixel host font draws),
 * demo workspace, 3 sizes. Asserts:
*   A6 no gap: a quiet (WORK) room tab ends at most 4 blank cells after its count
 *      (the operator 2026-09-17 "fix gap": the 6-cell slot left ~7 blanks);
 *   A1 a tab is its label plus a BOUNDED status slot: at most pad+glyph+name+count+2+pad
 *      cells (the old slot reserved " ·"+11 phase bytes, ~13 cells, per tab);
 *   A2 the demo's rooms all fit at 800x560: no "+N" badge;
 *   A3 tabs SHRINK before they hide: with the badge present every shown name is at
 *      the minimum cap, and a capped name ends in the ellipsis glyph, never a chop;
 *   A4 the status form is right-aligned: its last glyph sits one cell in from the
 *      tab's right edge, whatever the phase or question count;
 *   A5 widths stay put across phase/question flips while tabs are shrunk.
 * Fail-before: A1 29>23 cells for "Build room", A2 badge "+1" at 800, A3 no shrink,
 * A4 status trailed the count at the left. */
#define main liljack_entry
#include "../liljack_app/main.c"
#undef main
#include "../liljack_app/c_ansi.c"
#include <assert.h>
#include <locale.h>
#ifndef TAB_NAME_MIN_CELLS
#define TAB_NAME_MIN_CELLS 8   /* fail-before builds: the app had no cap */
#endif
static int checks,failed;
#define CHECK(c,...) do{checks++;if(!(c)){failed++;fprintf(stderr,"FAIL %s:%d ",__FILE__,__LINE__);fprintf(stderr,__VA_ARGS__);fputc('\n',stderr);}}while(0)
static const cell *at(int col,int row){return &state.canvas[row*state.cols+col];}
static Hit *find_hit(App *a,int kind,const char *id){for(int i=a->nhits-1;i>=0;i--)if(a->hits[i].kind==kind&&(!id||!strcmp(a->hits[i].id,id)))return &a->hits[i];return NULL;}
static void frame(App *a){lj_ansi_begin(a->w,a->h);render(a);}
static int cells_of(const char *s){int n=0;for(const char *q=s;*q;)n+=lj_render_cells_measure(nextcp(&q));return n;}
/* the most a tab may be: pad, glyph, space, name, " 64", status slot, pad */
static int ceiling_cells(const Room *r,int name_cells){
    return 1+cells_of(r?lj_theme_glyph(LJ_THEME_ROOM):lj_theme_glyph(LJ_THEME_STANDALONE))+1+name_cells+3+(r?2:0)+1;
}
static int last_glyph_col(const Hit *h){int row=h->r.y/CELLH,c0=h->r.x/CELLW,c1=c0+h->r.w/CELLW,last=-1;
    for(int c=c0;c<c1;c++){uint32_t cp=at(c,row)->cp;if(cp&&cp!=' ')last=c;}return last;}
static int row_has_cp(const Hit *h,uint32_t want){int row=h->r.y/CELLH,c0=h->r.x/CELLW,c1=c0+h->r.w/CELLW;
    for(int c=c0;c<c1;c++)if(at(c,row)->cp==want)return 1;return 0;}
static App *make(int w,int h,int extra){
    App *a=calloc(1,sizeof *a);assert(a);a->w=w;a->h=h;a->ansi=1;a->demo=1;a->split=-1;a->backend_in=a->backend_out=-1;
    for(int i=0;i<COUNT;i++)a->sessions[i].fd=-1;
    lj_render_resize(w,h);lj_dock_init(&a->dock);lj_media_init(&a->media);lj_popup_init(&a->popup);demo(a);
    const char *names[]={"Trio results review and next start","Kitty matrix","Region capture","Standards audit","Skill tree","lilJack's debug"};
    for(int i=0;i<extra&&i<6&&a->nroom<COUNT;i++){Room *r=&a->rooms[a->nroom++];memset(r,0,sizeof *r);snprintf(r->id,sizeof r->id,"r-x%d",i);copy(r->name,sizeof r->name,names[i]);}
    return a;
}
static void release(App *a){for(int i=0;i<a->nsession;i++)if(a->sessions[i].vt)lj_vt_free(a->sessions[i].vt);if(a->messages)json_object_put(a->messages);drafts_close(a);free(a);}
int main(void){
    setlocale(LC_CTYPE,"C.UTF-8");assert(!lj_render_init(1920,1080));
    const char *ell=lj_theme_glyph(LJ_THEME_ELLIPSIS);uint32_t ellcp=nextcp(&ell);
    int sizes[3][2]={{800,560},{1280,720},{1920,1080}};
    for(int s=0;s<3;s++){
        App *a=make(sizes[s][0],sizes[s][1],0);frame(a);
        /* A1 */
        for(int i=0;i<a->nhits;i++){Hit *h=&a->hits[i];if(h->kind!=H_GROUP)continue;Room *r=h->id[0]?room_by_id(a,h->id):NULL;
            int ceil=ceiling_cells(r,cells_of(r?r->name:"STANDALONE"));
            CHECK(h->r.w<=ceil*CELLW,"%dx%d tab '%s' is %d cells, ceiling %d",a->w,a->h,r?r->name:"STANDALONE",h->r.w/CELLW,ceil);}
        /* A6 */
        {Hit *h=find_hit(a,H_GROUP,"r-demo-research");
         if(h){int row=h->r.y/CELLH,c1=(h->r.x+h->r.w)/CELLW-1,blank=0;while(c1>=h->r.x/CELLW&&(!at(c1,row)->cp||at(c1,row)->cp==' ')){blank++;c1--;}
               CHECK(blank<=4,"%dx%d quiet Research tab ends in %d blank cells (want <=4)",a->w,a->h,blank);}}
        /* A2 */
        CHECK(find_hit(a,H_ROOM_MORE,NULL)==NULL,"%dx%d demo rooms overflow into a badge",a->w,a->h);
        /* A4 */
        for(int p=0;p<4;p++){
            const char *phases[]={"ALIGN","REVIEW","RESOLVED","ABCDEFGHIJK"};
            copy(a->rooms[1].phase,sizeof a->rooms[1].phase,phases[p]);a->rooms[1].questions=0;frame(a);
            Hit *h=find_hit(a,H_GROUP,"r-demo-research");CHECK(h!=NULL,"%dx%d research tab missing",a->w,a->h);
            if(h){int want=(h->r.x+h->r.w)/CELLW-2;CHECK(last_glyph_col(h)==want,"%dx%d phase %s ends at col %d, want %d",a->w,a->h,phases[p],last_glyph_col(h),want);}
        }
        a->rooms[1].phase[0]=0;a->rooms[1].questions=12345;frame(a);
        {Hit *h=find_hit(a,H_GROUP,"r-demo-research");if(h){int want=(h->r.x+h->r.w)/CELLW-2;CHECK(last_glyph_col(h)==want,"%dx%d questions end at col %d, want %d",a->w,a->h,last_glyph_col(h),want);}}
        release(a);
        /* A3 + A5: many long rooms */
        a=make(sizes[s][0],sizes[s][1],6);frame(a);
        Hit *more=find_hit(a,H_ROOM_MORE,NULL);int capped=0,shown=0;
        for(int i=0;i<a->nhits;i++){Hit *h=&a->hits[i];if(h->kind!=H_GROUP)continue;shown++;Room *r=h->id[0]?room_by_id(a,h->id):NULL;
            const char *name=r?r->name:"STANDALONE";int nc=cells_of(name),cap=nc<TAB_NAME_MIN_CELLS?nc:TAB_NAME_MIN_CELLS;
            if(more)CHECK(h->r.w<=ceiling_cells(r,cap)*CELLW,"%dx%d badge shown but tab '%s' not at the minimum cap (%d cells)",a->w,a->h,name,h->r.w/CELLW);
            if(h->r.w<ceiling_cells(r,nc)*CELLW&&nc>TAB_NAME_MIN_CELLS){capped++;CHECK(row_has_cp(h,ellcp),"%dx%d capped tab '%s' has no ellipsis",a->w,a->h,name);}
        }
        CHECK(shown>=(s?3:2),"%dx%d only %d tabs shown",a->w,a->h,shown);   /* 800 px: filter, +ROOM and the badge leave ~54 cells */
        CHECK(capped>0,"%dx%d the long room name was never capped",a->w,a->h);
        int w1[COUNT],k1=0;for(int i=0;i<a->nhits;i++)if(a->hits[i].kind==H_GROUP&&k1<COUNT)w1[k1++]=a->hits[i].r.x*10000+a->hits[i].r.w;
        for(int g=0;g<a->nroom;g++){a->rooms[g].questions=g*50;copy(a->rooms[g].phase,sizeof a->rooms[g].phase,g%2?"RESOLVED":"ALIGN");}
        frame(a);int w2[COUNT],k2=0;for(int i=0;i<a->nhits;i++)if(a->hits[i].kind==H_GROUP&&k2<COUNT)w2[k2++]=a->hits[i].r.x*10000+a->hits[i].r.w;
        CHECK(k1==k2&&!memcmp(w1,w2,sizeof(int)*k1),"%dx%d shrunk tabs moved on a live-state flip",a->w,a->h);
        release(a);
    }
    lj_render_close();
    fprintf(stderr,"%s tab-autoresize: %d checks, %d failed (bounded status slot, demo fits at 800, shrink before hide with ellipsis, right-aligned status, stable when shrunk) at 3 sizes\n",failed?"FAIL":"PASS",checks,failed);
    return failed?1:0;
}
