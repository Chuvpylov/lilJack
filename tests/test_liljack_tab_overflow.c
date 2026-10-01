/* tab-overflow — visual-polish-chrome item 1: a room tab is shown whole or not
 * at all; the rooms that do not fit are counted in a "+N ▾" badge at the strip's
 * right end that opens a rooms list (MENU_ROOMS). ANSI canvas, 3 sizes, demo
 * workspace plus extra rooms to force overflow at every width. Asserts:
 *   O1 no tab is chopped: every H_GROUP hit has its identity width at the frame's
 *      shared name cap (tab-autoresize: names shrink with an ellipsis first);
 *   O2 the badge exists iff rooms are hidden, and its number equals the count of
 *      filter-admitted rooms without a tab;
 *   O3 the badge opens MENU_ROOMS listing every admitted room; selecting a hidden
 *      one switches the workspace to it;
 *   O4 tab widths do not change when live state flips (questions/phase on a room);
 *   O5 the active tab is the raised surface (SURF2 bg) on the terminal path.
 * Fail-before: the strip truncated the last tab ("Rese") and had no badge. */
#define main liljack_entry
#include "../liljack_app/main.c"
#undef main
#include "../liljack_app/c_ansi.c"
#include <assert.h>
#include <locale.h>
static int checks,failed;
#define CHECK(c,...) do{checks++;if(!(c)){failed++;fprintf(stderr,"FAIL %s:%d ",__FILE__,__LINE__);fprintf(stderr,__VA_ARGS__);fputc('\n',stderr);}}while(0)
static const cell *at(int col,int row){return &state.canvas[row*state.cols+col];}
static void row_text(int row,char *out,size_t cap){size_t n=0;for(int c=0;c<state.cols&&n+4<cap;c++){uint32_t cp=at(c,row)->cp;if(!cp)out[n++]=' ';else if(cp<128)out[n++]=(char)cp;else n+=(size_t)snprintf(out+n,cap-n,"<%X>",cp);}out[n]=0;}
static Hit *find_hit(App *a,int kind,const char *id){for(int i=a->nhits-1;i>=0;i--)if(a->hits[i].kind==kind&&(!id||!strcmp(a->hits[i].id,id)))return &a->hits[i];return NULL;}
static void frame(App *a){lj_ansi_begin(a->w,a->h);render(a);}
int main(void){
    setlocale(LC_CTYPE,"C.UTF-8");assert(!lj_render_init(1920,1080));
    int sizes[3][2]={{800,560},{1280,720},{1920,1080}};
    for(int s=0;s<3;s++){
        App *a=calloc(1,sizeof *a);assert(a);a->w=sizes[s][0];a->h=sizes[s][1];a->ansi=1;a->demo=1;a->split=-1;a->backend_in=a->backend_out=-1;
        for(int i=0;i<COUNT;i++)a->sessions[i].fd=-1;
        lj_render_resize(a->w,a->h);lj_dock_init(&a->dock);lj_media_init(&a->media);lj_popup_init(&a->popup);demo(a);
        /* extra rooms so every width overflows */
        const char *names[]={"Trio review","demo bench","Kitty matrix","Region capture","Standards audit","Skill tree"};
        for(int i=0;i<6&&a->nroom<COUNT;i++){Room *r=&a->rooms[a->nroom++];memset(r,0,sizeof *r);snprintf(r->id,sizeof r->id,"r-demo-x%d",i);copy(r->name,sizeof r->name,names[i]);}
        frame(a);
        int admitted=1+a->nroom,tabs=0,chopped=0;
        for(int i=0;i<a->nhits;i++){Hit *h=&a->hits[i];if(h->kind!=H_GROUP)continue;tabs++;
            Room *r=h->id[0]?room_by_id(a,h->id):NULL;int want=room_tab_cells(r,a->tab_name_cap)*CELLW;
            if(h->r.w!=want)chopped++;}
        CHECK(chopped==0,"%dx%d %d of %d tabs chopped",a->w,a->h,chopped,tabs);
        Hit *more=find_hit(a,H_ROOM_MORE,NULL);int hidden=admitted-tabs;
        CHECK((hidden>0)==(more!=NULL),"%dx%d hidden=%d badge=%d",a->w,a->h,hidden,more!=NULL);
        if(more){char t[2048];row_text(more->r.y/CELLH,t,sizeof t);char want[16];snprintf(want,sizeof want,"+%d",hidden);
            CHECK(strstr(t,want)!=NULL,"%dx%d badge row '%s' lacks '%s'",a->w,a->h,t,want);
            /* O3 */
            Hit m=*more;activate(a,&m);CHECK(a->menu_kind==MENU_ROOMS,"%dx%d badge opened kind %d",a->w,a->h,a->menu_kind);
            frame(a);MenuRow rows[24];int n=menu_rows(a,rows,24);CHECK(n==admitted,"%dx%d rooms menu lists %d of %d",a->w,a->h,n,admitted);
            int last=n-1;Hit *item=NULL;for(int i=a->nhits-1;i>=0;i--)if(a->hits[i].kind==H_MENU_ITEM&&a->hits[i].index==last){item=&a->hits[i];break;}
            CHECK(item!=NULL,"%dx%d no hit for the last room row",a->w,a->h);
            if(item){Hit it=*item;activate(a,&it);CHECK(!strcmp(a->active_room,a->rooms[a->nroom-1].id),"%dx%d selecting the hidden room gave active_room '%s'",a->w,a->h,a->active_room);}
            frame(a);Hit *act=find_hit(a,H_GROUP,a->rooms[a->nroom-1].id);
            /* the newly active room may itself still be hidden: the badge lists it checked */
            if(!act){activate(a,&m);frame(a);n=menu_rows(a,rows,24);int ck=0;for(int i=0;i<n;i++)if(rows[i].checked&&rows[i].index==a->nroom-1)ck=1;CHECK(ck,"%dx%d active hidden room not checked in the menu",a->w,a->h);menu_close(a);}
            switch_workspace(a,"r-demo-build");
        }
        /* O4 widths never depend on live state */
        frame(a);int w1[COUNT],k1=0;for(int i=0;i<a->nhits;i++)if(a->hits[i].kind==H_GROUP&&k1<COUNT)w1[k1++]=a->hits[i].r.w;
        a->rooms[0].questions=3;copy(a->rooms[1].phase,sizeof a->rooms[1].phase,"ALIGN");frame(a);
        int w2[COUNT],k2=0;for(int i=0;i<a->nhits;i++)if(a->hits[i].kind==H_GROUP&&k2<COUNT)w2[k2++]=a->hits[i].r.w;
        CHECK(k1==k2&&!memcmp(w1,w2,sizeof(int)*k1),"%dx%d tab widths moved on a live-state flip (%d vs %d tabs)",a->w,a->h,k1,k2);
        a->rooms[0].questions=0;a->rooms[1].phase[0]=0;
        /* O5 active tab raised */
        frame(a);Hit *bt=find_hit(a,H_GROUP,"r-demo-build");CHECK(bt!=NULL,"%dx%d Build room tab missing",a->w,a->h);
        if(bt){const cell *c=at(bt->r.x/CELLW+1,bt->r.y/CELLH);CHECK((c->bg&0xffffff)==(SURF2&0xffffff),"%dx%d active tab bg %06x, wanted %06x",a->w,a->h,c->bg&0xffffff,SURF2&0xffffff);}
        for(int i=0;i<a->nsession;i++)if(a->sessions[i].vt)lj_vt_free(a->sessions[i].vt);if(a->messages)json_object_put(a->messages);drafts_close(a);free(a);
    }
    lj_render_close();
    fprintf(stderr,"%s tab-overflow: %d checks, %d failed (whole tabs only, +N badge count, rooms menu opens/selects, widths stable on live flips, active tab raised) at 3 sizes\n",failed?"FAIL":"PASS",checks,failed);
    return failed?1:0;
}
