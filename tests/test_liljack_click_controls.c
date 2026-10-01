/* Real controller clicks; backend requests are captured in memory, never sent. */
#define main liljack_entry
#include "../liljack_app/main.c"
#undef main
#include <assert.h>
static App *fixture(int w,int h,int ansi){
 App*a=calloc(1,sizeof *a);assert(a);a->w=w;a->h=h;a->demo=1;a->ansi=ansi;a->split=-1;
 a->backend_in=a->backend_out=-1;for(int i=0;i<COUNT;i++)a->sessions[i].fd=-1;
 lj_dock_init(&a->dock);lj_media_init(&a->media);lj_popup_init(&a->popup);demo(a);return a;
}
static void dispose(App*a){
 for(int i=0;i<a->nsession;i++)if(a->sessions[i].vt)lj_vt_free(a->sessions[i].vt);
 if(a->messages)json_object_put(a->messages);free(a->pending);drafts_close(a);free(a);
}
static Hit target(App*a,int kind,int index,const char *id){
 for(int i=0;i<a->nhits;i++)if(a->hits[i].kind==kind&&a->hits[i].index==index&&!strcmp(a->hits[i].id,id))return a->hits[i];
 assert(0);return (Hit){0};
}
static int overlap(lj_rect a,lj_rect b){return a.x<b.x+b.w&&b.x<a.x+a.w&&a.y<b.y+b.h&&b.y<a.y+a.h;}
static void click(App*a,Hit h,int last){
 int x=last?h.r.x+h.r.w-CELLW/2:h.r.x+h.r.w/2,y=h.r.y+h.r.h/2;
 SDL_Event e;memset(&e,0,sizeof e);e.type=SDL_MOUSEBUTTONDOWN;e.button.button=SDL_BUTTON_LEFT;e.button.x=x;e.button.y=y;
 event(a,&e);e.type=SDL_MOUSEBUTTONUP;event(a,&e);
}
int main(void){
 setlocale(LC_CTYPE,"C.UTF-8");assert(!lj_render_init(1280,720));int cases=0;
 const int kinds[]={H_AGENT_FOCUS,H_AGENT_DM,H_CONTROL,H_CONTROL,H_CONTROL,H_LEAD};
 const int indices[]={0,0,0,1,2,0};
 for(int size=0;size<3;size++)for(int ansi=0;ansi<2;ansi++)for(int last=0;last<2;last++){
  int w=size==2?1920:size?1280:800,h=size==2?1080:size?720:560;
  for(int k=0;k<6;k++){
   App*a=fixture(w,h,ansi);a->team=1;render(a);Hit hit=target(a,kinds[k],indices[k],"demo-0");
   assert(hit.r.x%CELLW==0&&hit.r.y%CELLH==0&&hit.r.w%CELLW==0&&hit.r.h==CELLH);
   for(int i=0;i<a->nhits;i++)if(!strcmp(a->hits[i].id,"demo-0")&&!(a->hits[i].kind==hit.kind&&a->hits[i].index==hit.index))assert(!overlap(hit.r,a->hits[i].r));
   /* Busy IPC queues JSON in a->pending. No pipe/server/agent is involved. */
   a->demo=0;a->busy=1;a->backend_in=1;click(a,hit,last);
   if(k==0)assert(!strcmp(a->focus,"demo-0")&&!a->team);
   else if(k==1)assert(!strcmp(a->chat_dest,"demo-0")&&!a->team);
   else if(k==4){
    assert(!strcmp(a->stop_confirm,"demo-0")&&!a->pending);
    a->demo=1;render(a);Hit confirmation=target(a,H_CONTROL,2,"demo-0");
    assert(!memcmp(&confirmation.r,&hit.r,sizeof hit.r));
    a->demo=0;click(a,hit,last); /* exact same original pixel, no target relocation */
    assert(a->pending);json_object*j=json_tokener_parse(a->pending);assert(j);
    assert(!strcmp(jstr(j,"operation"),"stop")&&json_object_get_boolean(jget(j,"confirmed")));
    json_object_put(j);
   }
   else{
    assert(a->pending);json_object*j=json_tokener_parse(a->pending);assert(j);
    assert(!strcmp(jstr(j,"session"),"demo-0"));
    if(k==5)assert(!strcmp(jstr(j,"action"),"room_lead"));
    else assert(!strcmp(jstr(j,"operation"),k==2?"submit":"interrupt"));
    json_object_put(j);
   }
   cases++;dispose(a);
  }
  for(int narrow=0;narrow<2;narrow++)for(int close=0;close<2;close++)for(int agent=0;agent<3;agent++){
   char id[32];snprintf(id,sizeof id,"demo-%d",agent);
   App*a=fixture(w,h,ansi);geometry(a);
   if(narrow)lj_dock_resize(&a->dock,a->dock.dividers[0].node,-50,-50);render(a);
   Hit role=target(a,H_CONTROLS,0,id),x=target(a,H_HIDE,0,id);assert(!overlap(role.r,x.r));
   assert(role.r.x%CELLW==0&&x.r.x%CELLW==0&&role.r.h==CELLH&&x.r.h==CELLH);
   click(a,close?x:role,last);
   if(close)assert(!dock_has(a,id));
   else assert(a->menu_kind==MENU_ROLE&&!strcmp(a->menu_target,id));
   cases++;dispose(a);
  }
 }
 printf("Click controls: %d centre/last-cell actions PASS, pixel+ANSI800/1280/1920, disjoint Team rows and normal/narrow ROLE/x\n",cases);
 lj_render_close();return 0;
}
