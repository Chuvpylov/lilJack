#define main liljack_entry
#include "../liljack_app/main.c"
#undef main
#include "../liljack_app/c_ansi.c"
#include <assert.h>

static int text_at(int col,int row,const char *word){
 for(int i=0;word[i];i++)if(state.canvas[row*state.cols+col+i].cp!=(unsigned char)word[i])return 0;
 return 1;
}
static void field_check(int x,int y,int cols,const char *source){
 int last=-1;for(int i=0;i<cols;i++){
  cell *c=&state.canvas[(y/CELLH)*state.cols+x/CELLW+i];
  assert(c->cp!=0xfffd);
  if(c->cp&&c->cp!=' ')last=i;
 }
 assert(last>=0);
 if(label_cells(source)>cols&&state.canvas[(y/CELLH)*state.cols+x/CELLW+last].cp!=0x2026){fprintf(stderr,"status-elide: field '%s' cols=%d label_cells=%d last=%d lastcp=U+%04X\n",source,cols,label_cells(source),last,state.canvas[(y/CELLH)*state.cols+x/CELLW+last].cp);fflush(stderr);}
 if(label_cells(source)>cols)assert(state.canvas[(y/CELLH)*state.cols+x/CELLW+last].cp==0x2026);
 else assert(state.canvas[(y/CELLH)*state.cols+x/CELLW+last].cp!=0x2026);
}
static void capture(App*a,const char*dir){
 if(!dir)return;lj_render_resize(a->w,a->h);
 for(int y=0;y<state.rows;y++)for(int x=0;x<state.cols;x++){
  cell*c=&state.canvas[y*state.cols+x];lj_render_rect(x*CELLW,y*CELLH,CELLW,CELLH,c->bg);
 }
 for(int y=0;y<state.rows;y++)for(int x=0;x<state.cols;x++){
  cell*c=&state.canvas[y*state.cols+x];
  if(c->cp&&c->cp!=' ')lj_render_glyph(x*CELLW,y*CELLH,c->cp,c->fg,c->wide==2?2:1);
  for(int k=0;k<3&&c->marks[k];k++)lj_render_glyph(x*CELLW,y*CELLH,c->marks[k],c->fg,c->wide==2?2:1);
 }
 char path[1024];snprintf(path,sizeof path,"%s/status-top-%d.png",dir,a->w);assert(!lj_render_save_png(path));
}
int main(void){
 setlocale(LC_CTYPE,"C.UTF-8");char words[80];
 assert(!strcmp(status_words(words,sizeof words,"one two three",8),"one two…"));
 assert(!strcmp(status_words(words,sizeof words,"one two three",7),"one…"));
 assert(!strcmp(status_words(words,sizeof words,"overlong",3),"…"));
 App*a=calloc(1,sizeof*a);assert(a);
 a->demo=a->ansi=1;a->split=-1;a->backend_in=a->backend_out=-1;
 for(int i=0;i<COUNT;i++)a->sessions[i].fd=-1;
 lj_dock_init(&a->dock);lj_media_init(&a->media);lj_popup_init(&a->popup);demo(a);a->room_tab=RT_STATUS;
 a->status_ribbon=json_object_new_object();json_object *fields=json_object_new_array();json_object_object_add(a->status_ribbon,"fields",fields);
 for(int i=0;i<30;i++){json_object *f=json_object_new_object();char label[120];snprintf(label,sizeof label,"Fixture metric %02d: rendering details and overflow evidence",i);jadd(f,"kind","FIXTURE");jadd(f,"text",label);json_object_array_add(fields,f);}

 copy(a->sessions[0].role,sizeof a->sessions[0].role,"reviewer");
 copy(a->sessions[0].unavailable,sizeof a->sessions[0].unavailable,"Building an intentionally overflowing explanation for this unavailable session");
 copy(a->sessions[1].task,sizeof a->sessions[1].task,"Model design and ecosystem with a deliberately long task description");
 copy(a->sessions[2].task,sizeof a->sessions[2].task,"Critical review of Unicode 界界 é and a very long trailing task description");
 for(int k=0;k<2;k++){
  a->w=k?1280:800;a->h=k?720:560;assert(!lj_render_init(a->w,a->h));render(a);
  lj_rect room={0};for(int i=0;i<a->dock.tile_count;i++)if(!strcmp(a->dock.tiles[i].id,"room"))room=a->dock.tiles[i].rect;
  lj_rect body=scrollbar_body((lj_rect){room.x+CELLW,room.y+2*CELLH,room.w-2*CELLW,room.h-3*CELLH});
  int title=-1;for(int row=body.y/CELLH;row<(body.y+body.h)/CELLH;row++)if(text_at(body.x/CELLW,row,"AGENTS"))title=row;
  assert(title>=0);int widths[4];status_columns((body.w-CELLW)/CELLW,widths);
  for(int i=0;i<3;i++){
   Session*ag=&a->sessions[i];char identity[128];snprintf(identity,sizeof identity,"%s %s",icon(ag->agent),ag->agent);
   const char*values[]={identity,ag->role,ag->unavailable[0]?"UNAVAILABLE":"replay",ag->unavailable[0]?ag->unavailable:ag->task};
   int x=body.x+CELLW,y=(title+1+i)*CELLH;
   for(int col=0;col<4;col++){
    field_check(x,y,widths[col],values[col]);
    if(col<3)assert(state.canvas[(y/CELLH)*state.cols+x/CELLW+widths[col]].cp==' ');
    x+=(widths[col]+1)*CELLW;
   }
  }
  int summary_y=body.y+CELLH,summary_x=body.x+CELLW,cols=(body.w-CELLW)/CELLW;
  field_check(summary_x,summary_y,cols,"Build room · WORK · agents are working their todos");
  char summary[300];status_words(summary,sizeof summary,"Build room · WORK · agents are working their todos",cols);
  if(label_cells("Build room · WORK · agents are working their todos")>cols){
   size_t end=strlen(summary)-3;assert(!strncmp(summary,"Build room · WORK · agents are working their todos",end));
   assert("Build room · WORK · agents are working their todos"[end]==' ');
  }
  capture(a,getenv("LILJACK_STATUS_CAPTURE_DIR"));
  printf("Status %dx%d: four rendered columns=%d/%d/%d/%d; each clipped field ends in ellipsis; whole-word room summary PASS\n",a->w,a->h,widths[0],widths[1],widths[2],widths[3]);
 }
 /* Narrow glyph budgets exercise actual ANSI writes, including a wide glyph
  * near the edge and a combining mark that must not consume another cell. */
 const char *unicode="界界 évaluation longer";
 for(int cols=1;cols<=18;cols++){
  lj_ansi_begin(800,560);lj_ansi_rect(0,0,800,560,BG);render_ansi=1;
  status_text((lj_rect){0,0,cols*CELLW,CELLH},0,0,unicode,TEXT,cols*CELLW);
  field_check(0,0,cols,unicode);assert(state.canvas[cols].cp==' ');
 }
 for(int i=0;i<a->nsession;i++)if(a->sessions[i].vt)lj_vt_free(a->sessions[i].vt);
 json_object_put(a->status_ribbon);json_object_put(a->messages);drafts_close(a);free(a);lj_render_close();puts("Status elision Unicode edge cases PASS");
}
