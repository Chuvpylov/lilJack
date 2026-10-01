/* Approved tabs: real pointer actions, stable slots, bounded overflow. */
#define LJ_NATIVE_HELPERS_ONLY
#include "test_liljack_native.c"
#undef LJ_NATIVE_HELPERS_ONLY
#define mouse_event ansi_parser_mouse_event
#include "../liljack_app/c_ansi.c"
#undef mouse_event
#include <assert.h>
static void pointer_click(App*a,Hit h){SDL_Event e={0};e.type=SDL_MOUSEBUTTONDOWN;e.button.button=SDL_BUTTON_LEFT;e.button.x=h.r.x+h.r.w/2;e.button.y=h.r.y+h.r.h/2;event(a,&e);e.type=SDL_MOUSEBUTTONUP;event(a,&e);}
static Hit find_tab(App*a,int kind,int index,const char*id){for(int i=0;i<a->nhits;i++)if(a->hits[i].kind==kind&&a->hits[i].index==index&&(!id||!strcmp(a->hits[i].id,id)))return a->hits[i];assert(!"missing tab hit");return(Hit){0};}
int main(void){
 setlocale(LC_ALL,"");assert(!SDL_Init(SDL_INIT_VIDEO));int cases=0;
 lj_rect native_slots[3][3];
 for(int z=0;z<3;z++)for(int ansi=0;ansi<2;ansi++){
  App*a=calloc(1,sizeof*a);assert(a);test_app=a;a->w=z==2?1920:z?1280:800;a->h=z==2?1080:z?720:560;a->demo=1;a->ansi=ansi;a->split=-1;a->mousex=a->mousey=-1;a->backend_in=a->backend_out=-1;for(int i=0;i<COUNT;i++)a->sessions[i].fd=-1;
  lj_dock_init(&a->dock);lj_media_init(&a->media);lj_popup_init(&a->popup);demo(a);assert(!lj_render_init(a->w,a->h));state.opened=1;state.sixel=1;state.cellw=10;state.cellh=20;render(a);
  Hit open=find_tab(a,H_OPEN_HERE,0,NULL);assert(open.r.w==12*CELLW&&open.r.x+open.r.w<=a->w);
  for(int i=0;i<3;i++){char id[20];snprintf(id,sizeof id,"demo-%d",i);Hit h=find_tab(a,H_SESSION,0,id);assert(h.r.x+h.r.w<=open.r.x-4*CELLW);if(!ansi)native_slots[z][i]=h.r;else assert(!memcmp(&native_slots[z][i],&h.r,sizeof h.r));}
  Hit selected=find_tab(a,H_SESSION,0,a->selected);const uint32_t*p=lj_render_pixels();assert((p[(size_t)selected.r.y*a->w+selected.r.x]&0xffffff)==CYAN);assert((p[(size_t)(selected.r.y+CELLH-1)*a->w+selected.r.x]&0xffffff)==PANEL);
  Hit second=find_tab(a,H_SESSION,0,"demo-1");assert((p[(size_t)(second.r.y+CELLH-1)*a->w+second.r.x]&0xffffff)==EDGE);
  /* Tabs are CELLS: no image may cover a text cell of the tab or agent rows (rows 1-2), and the
   * labels must be present in the cell canvas at the hit-rects, so the host font renders them. */
  if(ansi){for(int i=0;i<state.image_count;i++){image *im=&state.images[i];for(int y=im->row;y<im->row+im->rows;y++)for(int x=im->col;x<im->col+im->cols;x++)
     if((y==1||y==2)&&x>=0&&x<state.cols){cell *c=&state.canvas[(size_t)y*state.cols+x];assert(!(c->cp&&c->cp!=' '));}}
   int glyphs=0;for(int x=selected.r.x/CELLW;x<(selected.r.x+selected.r.w)/CELLW;x++){cell *c=&state.canvas[(size_t)(selected.r.y/CELLH)*state.cols+x];if(c->cp&&c->cp!=' ')glyphs++;}assert(glyphs>=4);}
  /* Every launch uses the menu's real H_MENU_ITEM route and a captured request. */
  for(int agent=0;agent<4;agent++){
   pointer_click(a,find_tab(a,H_OPEN_HERE,0,NULL));render(a);assert(a->menu_kind==MENU_OPEN_HERE);
   Hit item=find_tab(a,H_MENU_ITEM,agent,NULL);a->demo=0;a->busy=1;a->backend_in=1;pointer_click(a,item);assert(a->menu_kind==MENU_NONE&&a->pending);
   json_object*j=json_tokener_parse(a->pending);const char*names[]={"claude","codex","deepseek","shell"};assert(!strcmp(jstr(j,"action"),"create")&&!strcmp(jstr(j,"agent"),names[agent])&&!strcmp(jstr(j,"room"),a->active_room));json_object_put(j);free(a->pending);a->pending=NULL;a->demo=1;a->backend_in=-1;render(a);cases++;
  }
  /* Role/availability/state flips may change labels, never any neighbour rect. */
  lj_rect before[3];for(int i=0;i<3;i++){char id[20];snprintf(id,sizeof id,"demo-%d",i);before[i]=find_tab(a,H_SESSION,0,id).r;}
  for(int flip=0;flip<4;flip++){Session*s=session(a,"demo-0");copy(s->role,sizeof s->role,flip&1?"lead":"reviewer");copy(s->state,sizeof s->state,flip&2?"stopped":"running");copy(s->unavailable,sizeof s->unavailable,flip&1?"offline":"");render(a);for(int i=0;i<3;i++){char id[20];snprintf(id,sizeof id,"demo-%d",i);Hit h=find_tab(a,H_SESSION,0,id);assert(!memcmp(&h.r,&before[i],sizeof h.r));}cases++;}
  /* A crowded roster exposes one bounded, clickable +N action. */
  for(int i=4;i<20;i++){Session*s=&a->sessions[a->nsession++];memset(s,0,sizeof*s);s->fd=-1;s->seen=1;snprintf(s->id,sizeof s->id,"extra-%d",i);copy(s->agent,sizeof s->agent,"codex");copy(s->room_id,sizeof s->room_id,a->active_room);a->order[a->norder++]=a->nsession-1;}
  render(a);Hit more=find_tab(a,H_TEAM,0,NULL);assert(more.r.w==4*CELLW&&more.r.x+more.r.w==open.r.x);pointer_click(a,more);assert(a->team);cases++;
  for(int i=0;i<a->nsession;i++)if(a->sessions[i].vt)lj_vt_free(a->sessions[i].vt);if(a->messages)json_object_put(a->messages);drafts_close(a);free(a);test_app=NULL;lj_render_close();
 }
 printf("Tabs: %d menu/state/overflow cases PASS; identical native/sixel slots800/1280/1920, active baseline interrupted\n",cases);return 0;
}
