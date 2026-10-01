/* Tile selection (the operator 2026-09-13: "selection doesn't stay and it's ---- crossing
 * what was selected"): selected cells keep their glyphs and carry a highlight
 * background on the ANSI path; the highlight persists after release until the
 * next click; copy still happens on release. */
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
 /* the first demo tile with a text row: pick a row with at least 6 glyphs */
 Session *s=NULL;int row=-1,c0=-1,c1=-1;
 for(int i=0;i<COUNT&&!s;i++){Session *t=&a->sessions[i];if(!t->vt||!t->id[0]||t->grid.w<=0)continue;
  int vrows=t->grid.h/CELLH;for(int y=t->view_row;y<t->rows&&y<t->view_row+vrows&&row<0;y++){   /* a VISIBLE row: the viewport may be scrolled */const lj_cell *r=lj_vt_row(t->vt,y);int first=-1,last=-1,n=0;for(int x=0;x<t->cols;x++)if(r[x].ch&&r[x].ch!=' '){if(first<0)first=x;last=x;n++;}
   if(n>=6){s=t;row=y;c0=first;c1=last;}}}
 assert(s&&row>=0);
 uint32_t want[64];int nw=0;{const lj_cell *r=lj_vt_row(s->vt,row);for(int x=c0;x<=c1&&nw<64;x++)want[nw++]=r[x].ch;}
 /* press inside the tile at (row,c0), drag to (row,c1), like the pointer would */
 SDL_Event e={0};e.type=SDL_MOUSEBUTTONDOWN;e.button.button=SDL_BUTTON_LEFT;e.button.x=s->grid.x+c0*CELLW+2;e.button.y=s->grid.y+(row-s->view_row)*CELLH+2;event(a,&e);
 e.type=SDL_MOUSEMOTION;e.motion.x=s->grid.x+c1*CELLW+2;e.motion.y=e.button.y;event(a,&e);
 assert(a->selecting);render(a);
 int bad=0;uint32_t selbg=0;
 for(int x=c0;x<=c1;x++){const cell *c=canvas_at(s->grid.x+x*CELLW,s->grid.y+(row-s->view_row)*CELLH);
  if(c->cp!=want[x-c0]){bad++;if(bad==1)printf("selection: cell %d shows U+%04X, wanted U+%04X (a rule glyph replaced the text)\n",x,c->cp,want[x-c0]);}
  if(!selbg)selbg=c->bg;}
 const cell *outside=canvas_at(s->grid.x+c0*CELLW,s->grid.y+(row+1-s->view_row)*CELLH);
 int highlighted=selbg&&selbg!=outside->bg;
 printf("selection: during drag glyph mismatches=%d highlight bg=%06x (row below %06x) highlighted=%d\n",bad,selbg,outside->bg,highlighted);
 /* release: copy happens, highlight must stay */
 e.type=SDL_MOUSEBUTTONUP;e.button.button=SDL_BUTTON_LEFT;e.button.x=s->grid.x+c1*CELLW+2;e.button.y=s->grid.y+(row-s->view_row)*CELLH+2;event(a,&e);
 render(a);const cell *after=canvas_at(s->grid.x+c0*CELLW,s->grid.y+(row-s->view_row)*CELLH);int stays=after->bg==selbg&&after->cp==want[0];
 printf("selection: after release highlight stays=%d glyph intact=%d\n",stays,after->cp==want[0]);
 /* a click elsewhere clears it */
 e.type=SDL_MOUSEBUTTONDOWN;e.button.x=s->grid.x+c0*CELLW+2;e.button.y=s->grid.y+(row+2-s->view_row)*CELLH+2;event(a,&e);e.type=SDL_MOUSEBUTTONUP;event(a,&e);render(a);
 const cell *cleared=canvas_at(s->grid.x+c0*CELLW,s->grid.y+(row-s->view_row)*CELLH);int gone=cleared->bg!=selbg;
 printf("selection: after a click elsewhere highlight cleared=%d\n",gone);
 int ok=!bad&&highlighted&&stays&&gone;printf("selection: %s\n",ok?"PASS":"FAIL");return ok?0:1;
}
