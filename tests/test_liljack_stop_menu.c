/* Stop menu regression: real pointer route, isolated pipe, no live backend. */
#define main liljack_entry
#include "../liljack_app/main.c"
#undef main
#include <assert.h>
static Hit stop_hit(App*a,int kind,int index,const char*id){
 for(int i=0;i<a->nhits;i++)if(a->hits[i].kind==kind&&a->hits[i].index==index&&(!id||!strcmp(a->hits[i].id,id)))return a->hits[i];
 assert(!"missing hit");return(Hit){0};
}
static void click(App*a,Hit h,int edge){SDL_Event e={0};e.type=SDL_MOUSEBUTTONDOWN;e.button.button=SDL_BUTTON_LEFT;e.button.x=h.r.x+(edge?h.r.w-CELLW/2:h.r.w/2);e.button.y=h.r.y+h.r.h/2;event(a,&e);e.type=SDL_MOUSEBUTTONUP;event(a,&e);}
int main(void){
 setlocale(LC_CTYPE,"C.UTF-8");assert(!lj_render_init(1920,1080));int cases=0,failures=0;
 for(int size=0;size<3;size++)for(int ansi=0;ansi<2;ansi++)for(int busy=0;busy<2;busy++)for(int edge=0;edge<2;edge++){
  App*a=calloc(1,sizeof*a);assert(a);a->w=size==2?1920:size?1280:800;a->h=size==2?1080:size?720:560;a->demo=1;a->ansi=ansi;a->split=-1;a->backend_in=a->backend_out=-1;
  for(int i=0;i<COUNT;i++)a->sessions[i].fd=-1;
  lj_render_resize(a->w,a->h);lj_dock_init(&a->dock);lj_media_init(&a->media);lj_popup_init(&a->popup);demo(a);render(a);
  click(a,stop_hit(a,H_CONTROLS,0,"demo-0"),edge);render(a);assert(a->menu_kind==MENU_ROLE);
  MenuRow rows[16];int n=menu_rows(a,rows,16),idx=-1;for(int i=0;i<n;i++)if(!strcmp(rows[i].label,"stop this session"))idx=i;assert(idx>=0);
  Hit stop=stop_hit(a,H_MENU_ITEM,idx,NULL);assert(stop.r.x>=0&&stop.r.y>=0&&stop.r.x+stop.r.w<=a->w&&stop.r.y+stop.r.h<=a->h);
  const char*dir=getenv("LILJACK_STOP_CAPTURE_DIR");if(dir&&!ansi&&!busy&&!edge){char path[1024];snprintf(path,sizeof path,"%s/dropdown-%d.png",dir,a->w);assert(!lj_render_save_png(path));}
  int fds[2];assert(!pipe(fds));fcntl(fds[0],F_SETFL,O_NONBLOCK);a->demo=0;a->backend_in=fds[1];a->busy=busy;
  click(a,stop,edge);assert(a->menu_kind==MENU_NONE);
  char wire[2048]={0};if(busy){if(a->pending)snprintf(wire,sizeof wire,"%s",a->pending);}else {ssize_t got=read(fds[0],wire,sizeof wire-1);if(got>0)wire[got]=0;}
  if(!*wire){fprintf(stderr,"FAIL first click: %dx%d ansi=%d busy=%d edge=%d armed=%s no request\n",a->w,a->h,ansi,busy,edge,a->stop_confirm);failures++;}
  else {json_object*j=json_tokener_parse(wire);assert(j);assert(!strcmp(jstr(j,"action"),"control"));assert(!strcmp(jstr(j,"session"),"demo-0"));assert(!strcmp(jstr(j,"operation"),"stop"));assert(json_object_get_boolean(jget(j,"confirmed")));assert(!a->stop_confirm[0]);json_object_put(j);
  }
  assert(dock_has(a,"demo-0")); /* no optimistic close while request is pending */
  json_object*snap=json_object_new_object(),*sr=json_object_new_array(),*rr=json_object_new_array();
  for(int i=0;i<a->nsession;i++){Session*t=&a->sessions[i];json_object*j=json_object_new_object();jadd(j,"id",t->id);jadd(j,"agent",t->agent);jadd(j,"role",t->role);jadd(j,"room_id",t->room_id);jadd(j,"state",i?"running":"stopped");jadd(j,"tmux_name",i?t->name:"");json_object_array_add(sr,j);}
  for(int i=0;i<a->nroom;i++){json_object*j=json_object_new_object();jadd(j,"id",a->rooms[i].id);jadd(j,"name",a->rooms[i].name);json_object_array_add(rr,j);}
  json_object_object_add(snap,"sessions",sr);json_object_object_add(snap,"rooms",rr);json_object_object_add(snap,"messages",json_object_new_array());
  a->demo=1;copy(a->focus,sizeof a->focus,"demo-0");copy(a->full,sizeof a->full,"demo-0");
  apply_snapshot(a,snap);assert(dock_has(a,"demo-0")); /* unrelated refresh cannot acknowledge stop */
  jadd(snap,"control_sent","stop");jadd(snap,"target","demo-0");apply_snapshot(a,snap);
  Session*stopped=session(a,"demo-0");assert(stopped&&!strcmp(stopped->state,"stopped"));
  if(dock_has(a,"demo-0")||stopped->vt){fprintf(stderr,"FAIL successful stop acknowledgement retains tile/VT\n");failures++;}
  else {assert(!strcmp(a->focus,"room")&&!a->full[0]);assert(dock_has(a,"demo-1"));}
  json_object_put(snap);

  close(fds[0]);close(fds[1]);for(int i=0;i<a->nsession;i++)if(a->sessions[i].vt)lj_vt_free(a->sessions[i].vt);if(a->messages)json_object_put(a->messages);free(a->pending);drafts_close(a);free(a);cases++;
 }
 lj_render_close();printf("Stop menu: %d cases, %d routing/ack failures; pixel/ANSI, three sizes, idle/busy IPC, centre/edge\n",cases,failures);return failures?1:0;
}
