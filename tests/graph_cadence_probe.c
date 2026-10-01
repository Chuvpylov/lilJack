/* Production header/presenter with deterministic history, per-write trace. */
#define main liljack_entry
#include "../liljack_app/main.c"
#undef main
#include "../liljack_app/c_metrics.c"
#include <assert.h>
static FILE*logfile;static int frame_no,write_no;static int graph_bytes;
static ssize_t cadence_write(int fd,const void*buf,size_t n){
 if(fd==1&&logfile){const unsigned char*p=buf;int dcs=0;for(size_t i=0;i+1<n;i++)dcs+=p[i]==27&&p[i+1]=='P';
  fprintf(logfile,"frame=%d write=%d bytes=%zu dcs=%d prefix=",frame_no,write_no++,n,dcs);for(size_t i=0;i<n&&i<20;i++)fprintf(logfile,"%02x",p[i]);fputc('\n',logfile);if(dcs)graph_bytes+=(int)n;
 }
 return write(fd,buf,n);
}
#define write cadence_write
#define mouse_event ansi_parser_mouse_event
#include "../liljack_app/c_ansi.c"
#undef mouse_event
#undef write
int main(int argc,char**argv){
 assert(argc==2);setlocale(LC_ALL,"");assert(!SDL_Init(SDL_INIT_VIDEO));logfile=fopen(argv[1],"w");assert(logfile);FILE*sink=tmpfile();assert(sink);assert(dup2(fileno(sink),1)>=0);
 for(int size=0;size<3;size++){
  App*a=calloc(1,sizeof*a);assert(a);a->w=size==2?1920:size?1280:800;a->h=size==2?1080:size?720:560;a->demo=1;a->ansi=1;a->mousex=a->mousey=-1;a->split=-1;a->backend_in=a->backend_out=-1;for(int i=0;i<COUNT;i++)a->sessions[i].fd=-1;
  lj_dock_init(&a->dock);lj_media_init(&a->media);demo(a);assert(!lj_render_init(a->w,a->h));state.opened=1;state.sixel=1;state.cellw=10;state.cellh=20;state.min_ms=60;
  memset(state.shown,0,sizeof state.shown);memset(state.shown_cols,0,sizeof state.shown_cols);memset(&G,0,sizeof G);G.display_count=100;
  fprintf(logfile,"SIZE %d min_ms=60 metric_repaint_ms=%d\n",a->w,lj_metrics_repaint_ms());
  for(frame_no=0;frame_no<6;frame_no++){
   /* Frames0/1 unchanged; frame2 all plots change;3 immediately repeats;
    * frame4 changes GPU alone after cadence;5 idle. */
   if(frame_no==0||frame_no==2||frame_no==4){for(int m=0;m<LJ_METRIC_COUNT;m++)if(frame_no!=4||m==LJ_METRIC_GPU)for(int x=0;x<100;x++)G.display[m][x]=(float)((x+(frame_no+1)*4)%90)/100.f;G.paced_started=0;}
   if(frame_no==2||frame_no==4)SDL_Delay(110);
   render(a);write_no=0;graph_bytes=0;assert(lj_ansi_present());
   fprintf(logfile,"END frame=%d images_queued=%d writes=%d dcs_bytes=%d\n",frame_no,state.image_count,write_no,graph_bytes);
  }
  for(int i=0;i<a->nsession;i++)if(a->sessions[i].vt)lj_vt_free(a->sessions[i].vt);if(a->messages)json_object_put(a->messages);free(a);lj_render_close();
 }
 fclose(logfile);return 0;
}
