/* State flips must change glyphs, never this identity's or its neighbour's slot. */
#define main liljack_entry
#include "../liljack_app/main.c"
#undef main
#include "../liljack_app/c_ansi.c"
#include <assert.h>

static lj_rect slot(App *a,int kind,const char *id){
    for(int i=0;i<a->nhits;i++)if(a->hits[i].kind==kind&&!strcmp(a->hits[i].id,id))return a->hits[i].r;
    fprintf(stderr,"missing slot %d %s\n",kind,id);abort();
}
/* A room tab may be HIDDEN behind the "+N ▾" badge (visual-polish-chrome item 1:
 * a tab is shown whole or not at all). Its FORM must still be stable: hidden
 * stays hidden and the badge keeps its slot across every flip, so the identity
 * rule below compares the tab's rect OR its absence, plus the badge's rect. */
static lj_rect maybe_slot(App *a,int kind,const char *id){
    for(int i=0;i<a->nhits;i++)if(a->hits[i].kind==kind&&!strcmp(a->hits[i].id,id))return a->hits[i].r;
    for(int i=0;i<a->nhits;i++)if(a->hits[i].kind==H_ROOM_MORE)return (lj_rect){-1,-1,a->hits[i].r.x,a->hits[i].r.w};
    fprintf(stderr,"missing slot %d %s and no overflow badge\n",kind,id);abort();
}
static void same(lj_rect a,lj_rect b){assert(!memcmp(&a,&b,sizeof a));}
static void tabs(App *a,int width){a->nhits=0;lj_ansi_begin(width,560);draw_tabs(a,(lj_rect){0,0,width,CELLH});}
static void chips(App *a,int width){a->nhits=0;lj_ansi_begin(width,560);draw_agentbar(a,(lj_rect){0,CELLH,width,CELLH});}
int main(void){
    setlocale(LC_CTYPE,"C.UTF-8");render_ansi=1;App *a=calloc(1,sizeof *a);assert(a);
    a->nroom=2;copy(a->rooms[0].id,81,"r-first");copy(a->rooms[0].name,161,"First");
    copy(a->rooms[1].id,81,"r-next");copy(a->rooms[1].name,161,"Next");
    copy(a->active_room,81,"r-first");a->norder=a->nsession=2;
    for(int i=0;i<2;i++){a->order[i]=i;snprintf(a->sessions[i].id,81,"s-%d",i);copy(a->sessions[i].room_id,81,"r-first");}
    copy(a->sessions[0].agent,32,"codex");copy(a->sessions[1].agent,32,"claude");
    const char *phases[]={"","ALIGN","PLAN","WORK","REVIEW","RESOLVED","ABCDEFGHIJK"};
    int questions[]={0,1,99,INT_MAX};int widths[]={800,1280,1920};
    for(int w=0;w<3;w++){
        tabs(a,widths[w]);lj_rect first=slot(a,H_GROUP,"r-first"),next=maybe_slot(a,H_GROUP,"r-next");
        for(size_t p=0;p<sizeof phases/sizeof *phases;p++)for(size_t q=0;q<sizeof questions/sizeof *questions;q++){
            copy(a->rooms[0].phase,12,phases[p]);a->rooms[0].questions=questions[q];tabs(a,widths[w]);
            same(first,slot(a,H_GROUP,"r-first"));same(next,maybe_slot(a,H_GROUP,"r-next"));
        }
        puts("PASS room slots and neighbour across phase/question forms");
        a->rooms[0].questions=0;a->rooms[0].phase[0]=0;
        chips(a,widths[w]);first=slot(a,H_SESSION,"s-0");
        /* At 800px the fixed spawn controls leave room for only one wide chip. */
        if(w)next=slot(a,H_SESSION,"s-1");
        const char *roles[]={"worker","reviewer","lead"};
        const char *states[]={"running","stopped","exited","ended"};
        const char *errors[]={"","x","12345678901234","lookup failed indefinitely","日本語"};
        for(int r=0;r<3;r++)for(int s=0;s<4;s++)for(int e=0;e<5;e++){
            Session *ag=&a->sessions[0];copy(ag->role,20,roles[r]);copy(ag->state,40,states[s]);copy(ag->unavailable,120,errors[e]);
            ag->connected=(r+s+e)%2;chips(a,widths[w]);same(first,slot(a,H_SESSION,"s-0"));
            if(w)same(next,slot(a,H_SESSION,"s-1"));
        }
        memset(a->sessions[0].role,0,20);memset(a->sessions[0].state,0,40);memset(a->sessions[0].unavailable,0,120);
        printf("PASS %dpx chip slots across normal/dead/unavailable, role and connection flips\n",widths[w]);
    }
    free(a);lj_ansi_close();return 0;
}
