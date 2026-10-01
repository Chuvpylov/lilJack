/* ring-gutter-reservation (2026-09-13): THE LEAD RING NEVER SHARES A CELL WITH
 * TEXT. Measured on :0 in both WezTerm and owkTerm (docs/reports/
 * 2026-09-12-owkterm-composition.md): text written into a cell after a sixel
 * erases the sixel there, and in owkTerm a sixel pixel on a text cell replaces
 * the text with an image tile. So every cell the ring's four strips occupy
 * must be free of glyphs, wide continuations and marks, and must lie inside
 * the dock area — never on the chrome rows above it, never in the footer.
 *
 * (a) DEMO LAYOUT, three sizes: every strip cell of every lead ring is
 *     content-free and inside the dock area.
 * (b) LEAD TILE UNDER THE CHROME: the lead tile dropped on the dock's top edge;
 *     the chrome rows (tabs, agent chips) keep every glyph after a lead frame
 *     plus a march tick, and no strip cell lands on them. */
#define main liljack_entry
#include "../liljack_app/main.c"
#undef main
#include "../liljack_app/c_ansi.c"
#include <assert.h>

static int failures;
static int strip_cell(const pixel_border *b,int x,int y){
 return x==b->col||x==b->col+b->cols-1||y==b->row||y==b->row+b->rows-1;
}
static void check_rings(App *a,const char *tag){
 int dock_top=ANSI_DOCK_TOP/CELLH,footer_top=(a->h-ANSI_FOOTER_H)/CELLH,rings=0;
 for(int i=0;i<state.border_count;i++){
  pixel_border *b=&state.borders[i];if(b->separator)continue;rings++;
  int text=0,chrome=0,footer=0,total=0;
  for(int y=b->row;y<b->row+b->rows;y++)for(int x=b->col;x<b->col+b->cols;x++){
   if(!strip_cell(b,x,y))continue;total++;
   cell *c=&state.canvas[(size_t)y*state.cols+x];
   text+=c->content||c->wide||c->marks[0];
   chrome+=y<dock_top;footer+=y>=footer_top;
  }
  printf("%s %dx%d ring col=%d row=%d %dx%d inner=%d,%d strip cells=%d text=%d chrome=%d footer=%d\n",
         tag,a->w,a->h,b->col,b->row,b->cols,b->rows,b->inner_col,b->inner_row,total,text,chrome,footer);
  if(text||chrome||footer||!b->inner_col||!b->inner_row)failures++;
 }
 assert(rings>=1);
 /* PLACEHOLDERS DO NOT SURVIVE (lead 2026-09-13 01:41Z, "doubled border",
  * "colours flip"): dotted_border paints cell dots U+2022 as the coarse stand-in
  * and prepare_borders must replace every one of them once the strips are
  * encoded; a leftover dot is a second ring drawn in cells. */
 int leftover=0;
 for(int i=0;i<state.cols*state.rows;i++)leftover+=state.canvas[i].cp==0x2022;
 printf("%s %dx%d U+2022 placeholder cells left after prepare_borders=%d\n",tag,a->w,a->h,leftover);
 if(leftover)failures++;
}
int main(void){
 setlocale(LC_CTYPE,"C.UTF-8");assert(!lj_render_init(800,560));
 App*a=calloc(1,sizeof*a);assert(a);a->demo=a->ansi=1;a->split=-1;a->backend_in=a->backend_out=-1;
 for(int i=0;i<COUNT;i++)a->sessions[i].fd=-1;lj_dock_init(&a->dock);lj_media_init(&a->media);lj_popup_init(&a->popup);demo(a);
 int sizes[][2]={{800,560},{1280,720},{1920,1080}};
 /* (a) demo layout */
 for(unsigned z=0;z<sizeof sizes/sizeof sizes[0];z++){
  a->w=sizes[z][0];a->h=sizes[z][1];a->anim=0;copy(a->focus,sizeof a->focus,"demo-1");
  state.opened=1;state.sixel=1;state.cellw=10;state.cellh=20;render(a);
  for(int i=0;i<state.border_count;i++)state.borders[i].phase=0;prepare_borders();
  check_rings(a,"demo");state.opened=0;state.sixel=0;
 }
 /* (b) lead tile directly under the chrome rows */
 assert(lj_dock_drop(&a->dock,"demo-1","room",LJ_DOCK_TOP));
 for(unsigned z=0;z<sizeof sizes/sizeof sizes[0];z++){
  a->w=sizes[z][0];a->h=sizes[z][1];a->anim=0;copy(a->focus,sizeof a->focus,"demo-1");
  state.opened=1;state.sixel=1;state.cellw=10;state.cellh=20;render(a);
  int dock_top=ANSI_DOCK_TOP/CELLH;size_t n=(size_t)dock_top*state.cols;
  cell *before=malloc(n*sizeof(cell));assert(before);memcpy(before,state.canvas,n*sizeof(cell));
  for(int i=0;i<state.border_count;i++)state.borders[i].phase=0;prepare_borders();
  for(int i=0;i<state.border_count;i++)if(!state.borders[i].separator){state.borders[i].phase=1;prepare_border(&state.borders[i]);}
  int changed=0;
  for(size_t i=0;i<n;i++)changed+=before[i].cp!=state.canvas[i].cp||before[i].content!=state.canvas[i].content||memcmp(before[i].marks,state.canvas[i].marks,sizeof before[i].marks)!=0;
  printf("chrome %dx%d rows<%d cells changed by lead frame + tick=%d\n",a->w,a->h,dock_top,changed);
  if(changed)failures++;
  check_rings(a,"top-drop");free(before);state.opened=0;state.sixel=0;
 }
 for(int i=0;i<a->nsession;i++)if(a->sessions[i].vt)lj_vt_free(a->sessions[i].vt);if(a->messages)json_object_put(a->messages);drafts_close(a);free(a);lj_render_close();
 if(failures){printf("Ring gutter: %d violations — the ring shares cells with text/chrome FAIL\n",failures);return 1;}
 puts("Ring gutter: every lead ring owns content-free cells inside the dock at 800/1280/1920, chrome untouched PASS");return 0;
}
