/* Review-only mock: production source is included read-only; no click routing. */
#define main liljack_entry
#include "../liljack_app/main.c"
#undef main
#include <assert.h>
static void flat_tab(lj_rect r,const char*label,int active){
 lj_render_rect(r.x,r.y,r.w,r.h,PANEL);
 lj_render_rect(r.x,r.y+r.h-1,r.w,1,EDGE);
 if(active){lj_render_rect(r.x,r.y,r.w,2,CYAN);lj_render_rect(r.x,r.y+r.h-1,r.w,1,PANEL);}
 text(r.x+CELLW,r.y,label,active?TEXT:DIM,r.w-2*CELLW);
}
static void mock(App*a,int expanded){
 int y=CELLH,x=11*CELLW,right=a->w-8*CELLW;
 lj_render_rect(0,y,a->w,2*CELLH,PANEL);lj_render_rect(0,y+CELLH-1,a->w,1,EDGE);
 text(CELLW,y,"ACTIVE ▾",CYAN,10*CELLW);
 for(int g=-1;g<a->nroom&&x<right-4*CELLW;g++){
  Room*r=g<0?NULL:&a->rooms[g];char label[256];const char*id=r?r->id:"";
  snprintf(label,sizeof label,"%s %s %d",r?lj_theme_glyph(LJ_THEME_ROOM):lj_theme_glyph(LJ_THEME_STANDALONE),r?r->name:"STANDALONE",group_count(a,id));
  int w=(room_tab_slot_cells(r)+2)*CELLW;if(x+w>right)w=right-x;
  flat_tab((lj_rect){x,y,w,CELLH},label,!strcmp(a->active_room,id));x+=w;
 }
 text(right+CELLW,y,"+ ROOM",CYAN,7*CELLW);
 y+=CELLH;right=a->w-12*CELLW;x=0;
 lj_render_rect(0,y+CELLH-1,a->w,1,EDGE);
 int members=group_count(a,a->active_room),share=members?(right/CELLW)/members:0,shown=0;
 for(int i=0;i<a->norder;i++){
  Session*s=&a->sessions[a->order[i]];if(strcmp(s->room_id,a->active_room))continue;
  int w=(agent_chip_slot_cells(s)+2)*CELLW;if(w>share*CELLW)w=share*CELLW;if(x+w>right||w<6*CELLW)continue;
  char label[256];snprintf(label,sizeof label,"%s %s·%s",icon(s->agent),s->agent,!strcmp(s->role,"lead")?"LEAD":!strcmp(s->role,"reviewer")?"REV":"WRK");
  flat_tab((lj_rect){x,y,w,CELLH},label,!strcmp(a->selected,s->id));x+=w;shown++;
 }
 lj_render_rect(right,y,12*CELLW,CELLH,SURF2);text(right+CELLW,y,"OPEN HERE ▾",CYAN,11*CELLW);
 printf("mock width=%d launch_reserved_cells=12 session_budget_cells=%d visible=%d/%d\n",a->w,right/CELLW,shown,members);
 assert(shown==members);
 if(expanded){int my=y+CELLH;lj_render_rect(right,my,12*CELLW,4*CELLH,SURF2);const char*names[]={"CLAUDE","CODEX","DEEPSEEK","SHELL"};for(int i=0;i<4;i++)text(right+CELLW,my+i*CELLH,names[i],TEXT,11*CELLW);}
}
int main(int argc,char**argv){
 assert(argc==2);setlocale(LC_CTYPE,"C.UTF-8");assert(!lj_render_init(1920,1080));
 for(int size=0;size<3;size++){
  App*a=calloc(1,sizeof*a);assert(a);a->w=size==2?1920:size?1280:800;a->h=size==2?1080:size?720:560;a->demo=1;a->split=-1;a->backend_in=a->backend_out=-1;for(int i=0;i<COUNT;i++)a->sessions[i].fd=-1;
  lj_render_resize(a->w,a->h);lj_dock_init(&a->dock);lj_media_init(&a->media);lj_popup_init(&a->popup);demo(a);render(a);
  char path[1024];snprintf(path,sizeof path,"%s/before-%d.png",argv[1],a->w);assert(!lj_render_save_png(path));
  mock(a,0);snprintf(path,sizeof path,"%s/mock-%d.png",argv[1],a->w);assert(!lj_render_save_png(path));
  mock(a,1);snprintf(path,sizeof path,"%s/mock-open-here-%d.png",argv[1],a->w);assert(!lj_render_save_png(path));
  for(int i=0;i<a->nsession;i++)if(a->sessions[i].vt)lj_vt_free(a->sessions[i].vt);if(a->messages)json_object_put(a->messages);drafts_close(a);free(a);
 }
 lj_render_close();return 0;
}
