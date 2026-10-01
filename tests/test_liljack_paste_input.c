#define main liljack_entry
#include "../liljack_app/main.c"
#undef main
#include "../liljack_app/c_ansi.c"
#include <assert.h>
#include <glob.h>
static int master;
static void feed_bytes(const char*s,size_t n){while(n){ssize_t k=write(master,s,n);assert(k>0);s+=k;n-=(size_t)k;}}
static void reset_draft(App*a){a->draft[0]=0;a->draft_len=0;a->room_tab=RT_ROOM;copy(a->focus,sizeof a->focus,"room");}
static void pump(App*a,int wait_ms,int expected_pastes){
 int pastes=0,enters=0;long long end=milliseconds()+wait_ms;
 do{SDL_Event e;lj_ansi_message_input(message_input(a));
  if(lj_ansi_poll(&e)){pastes+=e.type==SDL_USEREVENT&&e.user.code==LJ_ANSI_PASTE_CODE;enters+=e.type==SDL_KEYDOWN&&e.key.keysym.sym==SDLK_RETURN;event(a,&e);}
  else{struct timespec pause={0,1000000};nanosleep(&pause,NULL);}
 }while(milliseconds()<end);
 assert(pastes==expected_pastes&&enters==0);
}
static void capture_paste(App *a){
 const char *dir=getenv("LILJACK_PASTE_CAPTURE_DIR");
 render(a);
 if(a->ansi){
  lj_rect input={0};for(int i=0;i<a->nhits;i++)if(a->hits[i].kind==H_ROOM)input=a->hits[i].r;
  assert(input.h==3*CELLH);
  int col=input.x/CELLW+1,row=input.y/CELLH+1;
  for(int i=0;i<9;i++)assert(state.canvas[row*state.cols+col+i].cp==(unsigned char)"last line"[i]);
  assert(state.canvas[row*state.cols+(input.x+input.w-2*CELLW)/CELLW].cp==0x23ce);
  assert(a->draft[a->draft_len-1]=='\n'&&a->draft[a->draft_len-2]=='\n');
  for(int y=0;y<state.rows;y++)for(int x=0;x<state.cols;x++){cell*c=&state.canvas[y*state.cols+x];lj_render_rect(x*CELLW,y*CELLH,CELLW,CELLH,c->bg);}
  for(int y=0;y<state.rows;y++)for(int x=0;x<state.cols;x++){
   cell*c=&state.canvas[y*state.cols+x];if(c->cp&&c->cp!=' ')lj_render_glyph(x*CELLW,y*CELLH,c->cp,c->fg,c->wide==2?2:1);
   for(int k=0;k<3&&c->marks[k];k++)lj_render_glyph(x*CELLW,y*CELLH,c->marks[k],c->fg,c->wide==2?2:1);
  }
 }
 if(!dir)return;char path[1024];snprintf(path,sizeof path,"%s/multiline-%s.png",dir,a->ansi?"ansi":"pixel");assert(!lj_render_save_png(path));
}
int main(void){
 setlocale(LC_CTYPE,"C.UTF-8");assert(!SDL_Init(SDL_INIT_VIDEO));
 int slave,saved_in=dup(0),saved_out=dup(1);assert(saved_in>=0&&saved_out>=0);
 struct winsize ws={.ws_col=80,.ws_row=28};assert(!openpty(&master,&slave,NULL,NULL,&ws));
 assert(dup2(slave,0)>=0&&dup2(slave,1)>=0);int w,h;assert(lj_ansi_open(&w,&h));
 /* Keep stdout on the PTY: dimensions() queries that descriptor. */
 App*a=calloc(1,sizeof*a);assert(a);a->demo=a->running=1;a->split=-1;a->backend_in=a->backend_out=-1;
 a->w=800;a->h=560;for(int i=0;i<COUNT;i++)a->sessions[i].fd=-1;
 lj_dock_init(&a->dock);lj_media_init(&a->media);lj_popup_init(&a->popup);assert(!lj_render_init(a->w,a->h));demo(a);
 const char *text="first line\nПривіт 世界\nlast line\n\n";
 char large[4097];for(int i=0;i<4096;i++)large[i]=(i%71==70)?'\n':(char)('a'+i%26);large[4096]=0;
 for(int mode=0;mode<2;mode++){
  a->ansi=mode;reset_draft(a);
  feed_bytes("\033[200~",6);feed_bytes(text,strlen(text));feed_bytes("\033[20",4);pump(a,10,0);
  feed_bytes("1~",2);pump(a,45,1);assert(!strcmp(a->draft,text));capture_paste(a);
  reset_draft(a);feed_bytes("\033[200~",6);feed_bytes(large,4096);feed_bytes("\033[201~",6);pump(a,60,1);assert(a->draft_len==4096&&!memcmp(a->draft,large,4097));
  reset_draft(a);feed_bytes(large,4096);pump(a,70,1);assert(a->draft_len==4096&&!memcmp(a->draft,large,4097));
  reset_draft(a);feed_bytes(text,strlen(text));pump(a,70,1);assert(!strcmp(a->draft,text));
  /* Missing framing does not turn pasted control bytes into app shortcuts. */
  reset_draft(a);char controls[66];memset(controls,'x',64);controls[64]=17;controls[65]=0;
  feed_bytes(controls,65);pump(a,60,1);assert(a->running&&!memcmp(a->draft,controls,66));
  fprintf(stderr,"Paste presenter%d: fragmented brackets, bracketed/raw4KB, multilineUTF8 and rawCtrlQ isolation PASS\n",mode);
 }
 reset_draft(a);feed_bytes(text,strlen(text));pump(a,10,0);
 feed_bytes("\033[<35;4;3M",strlen("\033[<35;4;3M"));pump(a,60,1);assert(!strcmp(a->draft,text));
 fprintf(stderr,"Raw paste excludes interleaved SGR mouse report PASS\n");
 /* One Enter remains a key when it arrives by itself. */
 lj_ansi_message_input(1);feed_bytes("\r",1);SDL_Event e;assert(lj_ansi_poll(&e)&&e.type==SDL_KEYDOWN&&e.key.keysym.sym==SDLK_RETURN);
 a->ansi=0;assert(!SDL_SetClipboardText(text));
 for(int shortcut=0;shortcut<4;shortcut++){
  reset_draft(a);memset(&e,0,sizeof e);
  if(shortcut==3){a->nhits=1;a->hits[0]=(Hit){.kind=H_ROOM,.r={0,0,100,40}};e.type=SDL_MOUSEBUTTONDOWN;e.button.button=SDL_BUTTON_MIDDLE;e.button.x=20;e.button.y=20;}
  else{e.type=SDL_KEYDOWN;e.key.keysym.sym=shortcut==2?SDLK_INSERT:SDLK_v;e.key.keysym.mod=shortcut==2?KMOD_SHIFT:shortcut==1?KMOD_CTRL|KMOD_SHIFT:KMOD_CTRL;}
  event(a,&e);assert(!strcmp(a->draft,text));
 }
 reset_draft(a);memset(&e,0,sizeof e);e.type=SDL_TEXTINPUT;strcpy(e.text.text,"first\nsecond");event(a,&e);assert(!strcmp(a->draft,"first\nsecond"));
 reset_draft(a);a->room_tab=RT_STATUS;e.type=SDL_USEREVENT;e.user.code=LJ_ANSI_PASTE_CODE;e.user.data1=strdup(text);event(a,&e);assert(!a->draft_len);
 reset_draft(a);a->room_tab=RT_REVIEW;clipboard_paste(a);assert(!a->draft_len);
 fprintf(stderr,"Clipboard Ctrl+V/Ctrl+Shift+V/Shift+Insert/middleclick, SDL text, hidden-composer guard and loneEnter PASS\n");
 /* Visual paste is routed by focus: canvas embeds a durable image; room opens
  * media and prepares a shareable reference. file:// paths are accepted. */
 char root[]="/tmp/liljack-visual-paste-XXXXXX";assert(mkdtemp(root));
 char source[PATH_MAX];snprintf(source,sizeof source,"%s/source.png",root);assert(!lj_render_save_png(source));
 copy(a->root,sizeof a->root,root);copy(a->active_room,sizeof a->active_room,"r-00000000000000000000000000000000");
 open_canvas(a);copy(a->focus,sizeof a->focus,"canvas");
 char uri[PATH_MAX+8];snprintf(uri,sizeof uri,"file://%s",source);input_paste(a,uri,1);assert(a->canvas.nstrokes==1&&a->canvas.strokes[0].kind==3);
 reset_draft(a);input_paste(a,source,1);assert(a->has_media&&strstr(a->draft,"media: ")==a->draft&&strstr(a->draft,"/media/"));
 /* F12 writes the current workspace into the same room media directory. */
 SDL_KeyboardEvent ke;memset(&ke,0,sizeof ke);ke.keysym.sym=SDLK_F12;key(a,&ke);
 char pattern[PATH_MAX];snprintf(pattern,sizeof pattern,"%s/rooms/%s/media/shot-*.png",root,a->active_room);glob_t g={0};assert(!glob(pattern,0,NULL,&g)&&g.gl_pathc==1);globfree(&g);
 fprintf(stderr,"Visual paste canvas/media routing and F12 shared screenshot PASS\n");
 /* the operator: "resolution is getting lower" while drawing. The pen keeps FULL
  * resolution; lag is handled by damage patches + frame ACK (sixel-damage test). */
 int sw=0,sh=0,rw=0,rh=0;canvas_source_size(2520,1060,1,&sw,&sh);canvas_source_size(2520,1060,0,&rw,&rh);
 assert(sw==rw&&sh==rh&&(long long)sw*sh<=1000000&&(long long)sw*sh>900000&&llabs((long long)sw*1060-(long long)sh*2520)<2520);
 fprintf(stderr,"Canvas drag keeps the resting source resolution (no low-res preview) PASS\n");
 char clean[PATH_MAX+32];snprintf(clean,sizeof clean,"rm -rf -- %s",root);assert(!system(clean));
 assert(dup2(slave,1)>=0);lj_ansi_close();assert(dup2(saved_in,0)>=0&&dup2(saved_out,1)>=0);close(saved_in);close(saved_out);close(master);close(slave);
 for(int i=0;i<a->nsession;i++)if(a->sessions[i].vt)lj_vt_free(a->sessions[i].vt);
 json_object_put(a->messages);drafts_close(a);lj_render_close();free(a);SDL_Quit();
 return 0;
}
