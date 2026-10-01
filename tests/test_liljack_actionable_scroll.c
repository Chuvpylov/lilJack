/* actionable-scroll — the operator 2026-09-16: "I can't scroll actionable todos".
 * Reproduced by codex (room message 2103): with many DMs, draw_dms used the whole
 * column and draw_todos got todo_rect.h = -40 at 1280x720, so the wheel branch's
 * inside(todo_rect) was never true. Also todo_height drifted (800 -> 820) while
 * scrolling the same 30 items, because a row's meta line counted only in view.
 * Production controller, SDL wheel events. For 0, 3 and 20 DMs at 3 sizes:
 *   T1 the todo list keeps a real body: todo_rect.h >= 3 rows;
 *   T2 a wheel over the todo list scrolls it (todo_scroll grows);
 *   T3 todo_height does not change as the list scrolls (same items);
 *   T4 the DM block says how many it could not show rather than eating the list. */
#define main liljack_application_main
#include "../liljack_app/main.c"
#undef main
#include "../liljack_app/c_ansi.c"
#include <assert.h>
static int checks,failed;
#define CHECK(c,...) do{checks++;if(!(c)){failed++;fprintf(stderr,"FAIL %s:%d ",__FILE__,__LINE__);fprintf(stderr,__VA_ARGS__);fputc('\n',stderr);}}while(0)
static void wheel(App *a,int x,int y,int dy){SDL_Event e={0};e.type=SDL_MOUSEMOTION;e.motion.x=x;e.motion.y=y;event(a,&e);
    e=(SDL_Event){0};e.type=SDL_MOUSEWHEEL;e.wheel.y=dy;e.wheel.direction=SDL_MOUSEWHEEL_NORMAL;event(a,&e);render(a);}
static int screen_has(const char *needle){for(int y=0;y<state.rows;y++){char row[1024];size_t n=0;for(int x=0;x<state.cols&&n<1000;x++){uint32_t cp=state.canvas[y*state.cols+x].cp;row[n++]=cp&&cp<128?(char)cp:' ';}row[n]=0;if(strstr(row,needle))return 1;}return 0;}
int main(void){
    setlocale(LC_CTYPE,"C.UTF-8");assert(!lj_render_init(1920,1080));
    int sizes[3][2]={{800,560},{1280,720},{1920,1080}},dms[3]={0,3,20};
    for(int s=0;s<3;s++)for(int d=0;d<3;d++){
        App *a=calloc(1,sizeof *a);assert(a);a->w=sizes[s][0];a->h=sizes[s][1];a->ansi=a->running=a->demo=1;a->split=-1;a->backend_in=a->backend_out=-1;a->mousex=a->mousey=-1;
        for(int i=0;i<COUNT;i++)a->sessions[i].fd=-1;
        lj_render_resize(a->w,a->h);lj_dock_init(&a->dock);lj_media_init(&a->media);lj_popup_init(&a->popup);demo(a);
        copy(a->focus,sizeof a->focus,"room");a->room_tab=RT_ACTION;
        if(a->room_todos)json_object_put(a->room_todos);a->room_todos=json_object_new_array();
        for(int i=0;i<30;i++){json_object *t=json_object_new_object();char id[30];snprintf(id,sizeof id,"task-%02d",i);jadd(t,"id",id);jadd(t,"title",id);jadd(t,"owner","codex");jadd(t,"state","active");json_object_array_add(a->room_todos,t);}
        if(a->room_dms)json_object_put(a->room_dms);a->room_dms=json_object_new_array();
        for(int i=0;i<dms[d];i++){json_object *m=json_object_new_object();jadd(m,"id","dm");jadd(m,"agent","codex");jadd(m,"text","message");jadd(m,"subject","subject");json_object_array_add(a->room_dms,m);}
        lj_ansi_begin(a->w,a->h);render(a);
        lj_rect tr=a->todo_rect;
        CHECK(tr.h>=3*CELLH,"%dx%d dms=%d todo_rect.h=%d (want >= %d)",a->w,a->h,dms[d],tr.h,3*CELLH);
        int h0=a->todo_height,s0=a->todo_scroll;
        if(tr.h>0)wheel(a,tr.x+CELLW,tr.y+CELLH/2,-1);
        CHECK(a->todo_scroll>s0,"%dx%d dms=%d wheel did not scroll todos (%d -> %d)",a->w,a->h,dms[d],s0,a->todo_scroll);
        for(int k=0;k<6;k++)if(tr.h>0)wheel(a,tr.x+CELLW,tr.y+CELLH/2,-1);
        CHECK(a->todo_height==h0,"%dx%d dms=%d todo_height drifted while scrolling (%d -> %d)",a->w,a->h,dms[d],h0,a->todo_height);
        if(dms[d]==20)CHECK(screen_has("more DM"),"%dx%d dms=20 the DM block does not say some DMs are not shown",a->w,a->h);
        for(int i=0;i<a->nsession;i++)if(a->sessions[i].vt)lj_vt_free(a->sessions[i].vt);if(a->messages)json_object_put(a->messages);drafts_close(a);free(a);
    }
    lj_render_close();
    fprintf(stderr,"%s actionable-scroll: %d checks, %d failed (todo body survives DMs, wheel scrolls, stable height, DM overflow named) at 3 sizes x 0/3/20 DMs\n",failed?"FAIL":"PASS",checks,failed);
    return failed?1:0;
}
