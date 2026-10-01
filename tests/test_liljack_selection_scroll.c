/* the operator 2026-09-13: "selector works strangely, not scrolling with the text it selects."
 * A selection is anchored to the TEXT: when the tile scrolls by N rows, the
 * highlight must sit on the same text N rows higher, not on the same screen rows. */
#define LJ_NATIVE_HELPERS_ONLY
#include "test_liljack_native.c"
#undef LJ_NATIVE_HELPERS_ONLY
#define mouse_event ansi_parser_mouse_event
#include "../liljack_app/c_ansi.c"
#undef mouse_event
#include <assert.h>
static const cell *canvas_at(int px,int py){return &state.canvas[(size_t)(py/CELLH)*state.cols+px/CELLW];}
int main(void){
 setlocale(LC_ALL,"");assert(!SDL_Init(SDL_INIT_VIDEO));
 App*a=calloc(1,sizeof*a);assert(a);test_app=a;a->w=1280;a->h=720;a->demo=1;a->ansi=1;a->split=-1;a->mousex=a->mousey=-1;a->backend_in=a->backend_out=-1;for(int i=0;i<COUNT;i++)a->sessions[i].fd=-1;
 lj_dock_init(&a->dock);lj_media_init(&a->media);lj_popup_init(&a->popup);demo(a);assert(!lj_render_init(a->w,a->h));state.opened=1;state.sixel=1;state.cellw=10;state.cellh=20;render(a);
 Session *s=NULL;int row=-1,c0=-1,c1=-1;
 for(int i=0;i<COUNT&&!s;i++){Session *t=&a->sessions[i];if(!t->vt||!t->id[0]||t->grid.w<=0)continue;int vrows=t->grid.h/CELLH;
  for(int y=t->view_row;y<t->rows&&y<t->view_row+vrows&&row<0;y++){const lj_cell *r=lj_vt_row(t->vt,y);int first=-1,last=-1,n=0;for(int x=0;x<t->cols;x++)if(r[x].ch&&r[x].ch!=' '){if(first<0)first=x;last=x;n++;}
   if(n>=6&&y>=t->view_row+1){s=t;row=y;c0=first;c1=last;}}}
 assert(s&&row>=0);
 uint32_t want[64];int nw=0;{const lj_cell *r=lj_vt_row(s->vt,row);for(int x=c0;x<=c1&&nw<64;x++)want[nw++]=r[x].ch;}
 SDL_Event e={0};e.type=SDL_MOUSEBUTTONDOWN;e.button.button=SDL_BUTTON_LEFT;e.button.x=s->grid.x+c0*CELLW+2;e.button.y=s->grid.y+(row-s->view_row)*CELLH+2;event(a,&e);
 e.type=SDL_MOUSEMOTION;e.motion.x=s->grid.x+c1*CELLW+2;e.motion.y=e.button.y;event(a,&e);
 e.type=SDL_MOUSEBUTTONUP;e.button.button=SDL_BUTTON_LEFT;e.button.x=s->grid.x+c1*CELLW+2;e.button.y=s->grid.y+(row-s->view_row)*CELLH+2;event(a,&e);
 render(a);uint32_t selbg=canvas_at(s->grid.x+c0*CELLW,s->grid.y+(row-s->view_row)*CELLH)->bg;
 /* the tile scrolls by N: cursor to the last row, N newlines */
 const int N=3;lj_vt_feed(s->vt,(const unsigned char*)"\033[999;1H",8);for(int i=0;i<N;i++)lj_vt_feed(s->vt,(const unsigned char*)"\r\n",2);
 render(a);
 /* where is the text now? find the row whose glyphs equal `want` at c0..c1 */
 int text_row=-1;for(int y=0;y<s->rows;y++){const lj_cell *r=lj_vt_row(s->vt,y);int ok=1;for(int k=0;k<nw;k++)if(r[c0+k].ch!=want[k]){ok=0;break;}if(ok){text_row=y;break;}}
 int vrows=s->grid.h/CELLH;
 printf("selection-scroll: selected row %d, after %d scrolled lines the text is at row %d (view_row %d, %d visible rows)\n",row,N,text_row,s->view_row,vrows);
 int on_text=0,on_old=0;
 if(text_row>=s->view_row&&text_row<s->view_row+vrows){const cell *c=canvas_at(s->grid.x+c0*CELLW,s->grid.y+(text_row-s->view_row)*CELLH);on_text=c->bg==selbg&&c->cp==want[0];}
 {const cell *c=canvas_at(s->grid.x+c0*CELLW,s->grid.y+(row-s->view_row)*CELLH);on_old=c->bg==selbg;}
 printf("selection-scroll: highlight follows the text=%d, highlight left on the old screen row=%d\n",on_text,on_old);
 int ok=text_row>=0&&on_text&&!on_old;printf("selection-scroll: %s\n",ok?"PASS":"FAIL");return ok?0:1;
}
