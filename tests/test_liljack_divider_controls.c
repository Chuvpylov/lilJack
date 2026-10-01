/* Demo-only real event dispatch; no backend or live-session operations. */
#define main liljack_entry
#include "../liljack_app/main.c"
#undef main
#include <assert.h>
static void pointer(App*a,Uint32 type,int x,int y){
 SDL_Event e={0};e.type=type;if(type==SDL_MOUSEMOTION){e.motion.x=x;e.motion.y=y;e.motion.state=SDL_BUTTON_LMASK;}
 else{e.button.x=x;e.button.y=y;e.button.button=SDL_BUTTON_LEFT;}event(a,&e);
}
static void build_topology(App*a,int kind){
 lj_dock_init(&a->dock);a->split=-1;
 if(kind==2){assert(lj_dock_drop(&a->dock,"demo-0","room",LJ_DOCK_TOP));assert(lj_dock_drop(&a->dock,"demo-1","room",LJ_DOCK_LEFT));}
 else{assert(lj_dock_drop(&a->dock,"demo-0","room",LJ_DOCK_LEFT));assert(lj_dock_drop(&a->dock,"demo-1","demo-0",LJ_DOCK_BOTTOM));assert(lj_dock_drop(&a->dock,"demo-2","room",LJ_DOCK_TOP));
  if(kind==1)a->dock.nodes[a->dock.nodes[a->dock.root].a].ratio=.625;
 }
 render(a);
}
#ifndef H6_CAPTURE
#define h6_capture(a,state) ((void)0)
#endif
int main(int argc,char **argv){
 (void)argc;(void)argv;setlocale(LC_CTYPE,"C.UTF-8");assert(!lj_render_init(800,560));int cases=0;
 for(int size=0;size<3;size++)for(int ansi=0;ansi<2;ansi++)for(int kind=0;kind<3;kind++){
  App*a=calloc(1,sizeof *a);assert(a);a->w=size==0?800:size==1?1280:1920;a->h=size==0?560:size==1?720:1080;
  a->demo=1;a->ansi=ansi;a->backend_in=a->backend_out=-1;a->split=-1;
  for(int i=0;i<COUNT;i++)a->sessions[i].fd=-1;
  lj_media_init(&a->media);lj_popup_init(&a->popup);lj_dock_init(&a->dock);demo(a);build_topology(a,kind);
  h6_capture(a,"default");
  if(kind==0){
   /* Reconstruct the old one-node drag for before/after visual evidence. */
   lj_dock save=a->dock;lj_dock_divider v=a->dock.dividers[1];
   a->dock.nodes[v.node].ratio=(double)(v.logical.y-v.area.y+40)/(v.area.h-v.logical.h);
   render(a);h6_capture(a,"before-independent-drag");a->dock=save;render(a);
  }
  int selected=kind==2?0:1;lj_dock_divider v=a->dock.dividers[selected];
  int x=v.rect.x+v.rect.w/2,y=v.rect.y+v.rect.h/2;
  if(kind==2)x=v.rect.x+v.rect.w/4;
  pointer(a,SDL_MOUSEMOTION,x,y);render(a);
  assert(divider_at(a,x,y)==selected);assert(divider_hot(a,selected));
  if(kind==0)assert(divider_hot(a,2));
  if(kind==1)assert(!divider_hot(a,2));
  h6_capture(a,"hover");
  pointer(a,SDL_MOUSEBUTTONDOWN,x,y);assert(a->split==v.node);
  assert(a->split_drag.count==(kind==0?2:1));
  lj_dock_tile before[LJ_DOCK_MAX_LEAVES];int count=a->dock.tile_count;memcpy(before,a->dock.tiles,sizeof before);
  pointer(a,SDL_MOUSEMOTION,x,y);render(a);
  for(int i=0;i<count;i++)assert(!memcmp(&before[i].rect,&a->dock.tiles[i].rect,sizeof(lj_rect)));
  pointer(a,SDL_MOUSEMOTION,x,y+40);render(a);h6_capture(a,"drag");
  assert(divider_hot(a,selected));
  if(kind==0){assert(divider_hot(a,2));assert(a->dock.dividers[1].rect.y==a->dock.dividers[2].rect.y);}
  if(kind==1)for(int i=0;i<count;i++)if(!strcmp(before[i].id,"demo-2")||!strcmp(before[i].id,"room"))assert(!memcmp(&before[i].rect,&a->dock.tiles[i].rect,sizeof(lj_rect)));
  if(kind==2){int dw=a->dock.dividers[0].rect.w,want=a->w-2*CELLW;assert(dw<=want&&dw>=want-CELLW);}   /* uniform window margin; width snapped to the cell grid */
  pointer(a,SDL_MOUSEBUTTONUP,x,y+40);assert(a->split==-1);
  if(kind==2){
   /* At an overlapping snapped T-junction, the smaller local split wins. */
   lj_rect parent=hitrect(a->dock.dividers[0].rect),local=hitrect(a->dock.dividers[1].rect);
   int ix=parent.x>local.x?parent.x:local.x,iy=parent.y>local.y?parent.y:local.y;
   if(inside(parent,ix,iy)&&inside(local,ix,iy)){assert(divider_at(a,ix,iy)==1);cases++;}
  }
  cases++;for(int i=0;i<a->nsession;i++)if(a->sessions[i].vt)lj_vt_free(a->sessions[i].vt);
  if(a->messages)json_object_put(a->messages);drafts_close(a);free(a);
 }
 printf("H6 real controller: %d topology/junction cases PASS, pixel+ANSI800/1280/1920\n",cases);lj_render_close();return 0;
}
