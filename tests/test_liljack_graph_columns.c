/* Finite low samples must remain distinguishable from unavailable samples. */
#define LJ_NATIVE_HELPERS_ONLY
#include "test_liljack_native.c"
#undef LJ_NATIVE_HELPERS_ONLY
#include "../liljack_app/c_metrics.c"
#define mouse_event ansi_parser_mouse_event
#include "../liljack_app/c_ansi.c"
#undef mouse_event
#define decode graph_decode
#define main existing_sixel_test_main
#undef CHECK
#include "test_liljack_sixel.c"
#undef main
#undef decode
int main(void){
    CHECK(setlocale(LC_ALL,"")!=NULL);CHECK(SDL_Init(SDL_INIT_VIDEO)==0);
    App *a=calloc(1,sizeof(*a));CHECK(a);test_app=a;
    a->running=1;a->demo=1;a->backend_in=a->backend_out=-1;a->split=-1;
    for(int i=0;i<COUNT;i++)a->sessions[i].fd=-1;
    lj_dock_init(&a->dock);lj_media_init(&a->media);demo(a);a->mousex=a->mousey=-1;
    const int widths[]={800,1280,1920},heights[]={560,720,1080};int failures=0;
    const char *dir=getenv("LILJACK_GRAPH_CAPTURE_DIR");
    int saved=dup(1);FILE *wirefile=tmpfile();CHECK(saved>=0&&wirefile);
    char *wire=malloc(SIXEL_CAP);uint32_t *scaled=NULL;size_t scaledn=0;
    for(int z=0;z<3;z++){
        a->w=widths[z];a->h=heights[z];CHECK(lj_render_init(a->w,a->h)==0);
        int pw=LJ_RIBBON_SPARK*CELLW,ph=CELLH;
        memset(&G,0,sizeof G);G.display_count=pw+20;
        for(int m=0;m<LJ_METRIC_COUNT;m++)for(int i=0;i<G.display_count;i++){
            int x=i-20,c=x/CELLW;
            G.display[m][i]=x<0?1.f:c==0?0.f:c==1?.001f:c==2?NAN:(float)(x-3*CELLW)/(pw-3*CELLW-1);
        }
        G.paced_started=0;
        int blank=0,bad_nan=0,bad_cells=0,bad_wire=0;
        state.opened=1;state.cellw=10;state.cellh=20;a->ansi=1;
        for(int mode=0;mode<2;mode++){
            state.sixel=mode;render(a);Header hdr=header_layout(a);CHECK(hdr.rung==2);
            for(int m=0;m<3;m++)for(int c=0;c<LJ_RIBBON_SPARK;c++){
                int x=(hdr.graphs_x+m*hdr.per*CELLW+9*CELLW)/CELLW+c;
                uint32_t cp=state.canvas[x].cp;
                bad_cells+=c==2?(cp&&cp!=' '):!(cp>=0x2581&&cp<=0x2588);
            }
            if(mode)for(int m=0;m<3;m++)for(int x=0;x<pw;x++){
                int ink=0;for(int y=0;y<ph;y++)ink+=!!(a->header_plot[m][y*pw+x]>>24);
                if(x/CELLW==2)bad_nan+=!!ink;else blank+=!ink;
            }
            /* Save cells and decoded sixel at the application's logical grid. */
            if(dir){
                for(int y=0;y<state.rows;y++)for(int x=0;x<state.cols;x++){
                    cell *c=&state.canvas[y*state.cols+x];lj_render_rect(x*CELLW,y*CELLH,CELLW,CELLH,c->bg);
                    if(c->cp&&c->cp!=' ')lj_render_glyph(x*CELLW,y*CELLH,c->cp,c->fg,c->wide==2?2:1);
                }
                if(mode)for(int k=0;k<state.image_count;k++){
                    image *im=&state.images[k];if(im->row||im->cols!=LJ_RIBBON_SPARK)continue;
                    CHECK(dup2(fileno(wirefile),1)>=0);CHECK(lseek(1,0,SEEK_SET)==0);CHECK(!ftruncate(1,0));
                    int n=emit_image(im,k,wire,&scaled,&scaledn);CHECK(n>0);CHECK(dup2(saved,1)>=0);
                    uint32_t pixels[LJ_RIBBON_SPARK*CELLW*CELLH];graph_decode(wire,n,pixels,pw,ph);
                    for(int y=0;y<ph;y++)for(int x=0;x<pw;x++)lj_render_rect(im->col*CELLW+x,y,1,1,pixels[y*pw+x]);
                }
                char path[PATH_MAX];snprintf(path,sizeof path,"%s/%s-%dx%d.png",dir,mode?"sixel-decoded":"cells",a->w,a->h);CHECK(!lj_render_save_png(path));
            }
            if(mode){
                state.cellw=17;state.cellh=39;
                for(int k=0;k<state.image_count;k++){
                    image *im=&state.images[k];if(im->row||im->cols!=LJ_RIBBON_SPARK)continue;
                    CHECK(dup2(fileno(wirefile),1)>=0);CHECK(lseek(1,0,SEEK_SET)==0);CHECK(!ftruncate(1,0));
                    int n=emit_image(im,k,wire,&scaled,&scaledn);CHECK(n>0);CHECK(dup2(saved,1)>=0);
                    int tw=im->cols*17,th=39;uint32_t *pixels=calloc((size_t)tw*th,4);CHECK(pixels);graph_decode(wire,n,pixels,tw,th);
                    for(int x=0;x<tw;x++){
                        int sx=x*pw/tw,ink=0;for(int y=0;y<th;y++)ink+=pixels[y*tw+x]!=0;
                        bad_wire+=(sx/CELLW==2)?!!ink:!ink;
                    }free(pixels);
                }
            }
        }
        printf("width=%d finite_blank_columns=%d NAN_ink=%d fallback_errors=%d sixel_errors=%d\n",a->w,blank,bad_nan,bad_cells,bad_wire);
        failures+=blank+bad_nan+bad_cells+bad_wire;
    }
    state.opened=0;lj_ansi_close();a->ansi=0;free(wire);free(scaled);fclose(wirefile);close(saved);test_cleanup();
    return failures?1:0;
}
