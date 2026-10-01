#define LJ_NATIVE_HELPERS_ONLY
#include "test_liljack_native.c"
#undef LJ_NATIVE_HELPERS_ONLY
#include "../liljack_app/c_metrics.c"

#define mouse_event ansi_parser_mouse_event
#include "../liljack_app/c_ansi.c"
#undef mouse_event
static uint32_t reg[256];
static int reg_set[256];

static void put_sixel(uint32_t *dst, int w, int h, int *col, int band, int color, int mask) {
    for (int row = 0; row < 6; row++) {
        if (!(mask & (1 << row))) continue;
        int py = band + row, px = *col;
        if (px >= 0 && px < w && py >= 0 && py < h && reg_set[color])
            dst[py * w + px] = reg[color];
    }
    (*col)++;
}

static void decode_sixel(const char *s, int n, uint32_t *dst, int w, int h) {
    memset(dst, 0, (size_t)w * h * 4);
    memset(reg_set, 0, sizeof reg_set);
    int i = 0;
    /* find DCS ESC P ... q */
    while (i + 1 < n && !(s[i] == 0x1b && s[i + 1] == 'P')) i++;
    i += 2;
    while (i < n && s[i] != 'q') i++;
    i++;                                   /* past q */
    int col = 0, band = 0, color = 0;
    while (i < n) {
        char c = s[i];
        if (c == 0x1b && i + 1 < n && s[i + 1] == '\\') break;   /* ST */
        if (c == '#') {
            i++;
            int num = 0;
            while (i < n && s[i] >= '0' && s[i] <= '9') num = num * 10 + (s[i++] - '0');
            if (i + 1 < n && s[i] == ';') {
                /* colour definition #n;2;r;g;b */
                int r = 0, g = 0, b = 0;
                int mode = 0;
                while (i < n && s[i] != '#' && s[i] != '$' && s[i] != '-' && s[i] != '"') {
                    if (s[i] >= '0' && s[i] <= '9') {
                        int v = 0;
                        while (i < n && s[i] >= '0' && s[i] <= '9') v = v * 10 + (s[i++] - '0');
                        if (mode == 2) r = v; else if (mode == 3) g = v; else if (mode == 4) b = v;
                    } else if (s[i] == ';') { mode++; i++; }
                    else i++;
                }
                reg[num] = (uint32_t)((r * 255 + 50) / 100) << 16
                         | (uint32_t)((g * 255 + 50) / 100) << 8
                         | (uint32_t)((b * 255 + 50) / 100);
                reg_set[num] = 1;
                continue;
            }
            color = num;
            continue;
        }
        if (c == '$') { col = 0; i++; continue; }
        if (c == '-') { band += 6; col = 0; i++; continue; }
        if (c == '"') { i++; continue; }                          /* raster attr */
        if (c == '!') {
            i++;
            int cnt = 0;
            while (i < n && s[i] >= '0' && s[i] <= '9') cnt = cnt * 10 + (s[i++] - '0');
            if (i < n) { int mask = s[i++] - 63; for (int k = 0; k < cnt; k++) put_sixel(dst, w, h, &col, band, color, mask); }
            continue;
        }
        if (c >= 63 && c <= 126) { i++; put_sixel(dst, w, h, &col, band, color, c - 63); continue; }
        i++;
    }
}

#include <assert.h>
static uint32_t *frame_raster(pixel_border*b,int *width,int *height){
 int cw,ch;lj_ansi_cell_pixels(&cw,&ch);int w=b->cols*cw,h=b->rows*ch;*width=w;*height=h;
 uint32_t *all=calloc((size_t)w*h,sizeof *all);assert(all);size_t at=0;
 for(int side=0;side<4;side++){
  while(at+1<b->bytes&&!(b->wire[at]==27&&b->wire[at+1]=='P'))at++;
  assert(at+1<b->bytes);size_t end=at+2;while(end+1<b->bytes&&!(b->wire[end]==27&&b->wire[end+1]=='\\'))end++;
  assert(end+1<b->bytes);
  int ox=side==3?w-cw:0,oy=side==1?h-ch:side>=2?ch:0;
  int sw=side<2?w:cw,sh=side<2?ch:h-2*ch;assert(sh>0);
  uint32_t *part=calloc((size_t)sw*sh,sizeof *part);assert(part);decode_sixel(b->wire+at,(int)(end+2-at),part,sw,sh);
  for(int y=0;y<sh;y++)for(int x=0;x<sw;x++)if(part[y*sw+x])all[(oy+y)*w+ox+x]=part[y*sw+x];
  free(part);at=end+2;
 }return all;
}
/* Paint ANSI cells, then decode bytes from the real deferred image emitter. */
static void ansi_capture(App *a,const char *dir,const char *name){
    prepare_borders();
    lj_render_resize(a->w,a->h);
    for(int y=0;y<state.rows;y++)for(int x=0;x<state.cols;x++){
        cell *c=&state.canvas[y*state.cols+x];
        lj_render_rect(x*CELLW,y*CELLH,CELLW,CELLH,c->bg);
        if(c->cp&&c->cp!=' ')lj_render_glyph(x*CELLW,y*CELLH,c->cp,c->fg,c->wide==2?2:1);
        for(int k=0;k<3&&c->marks[k];k++)lj_render_glyph(x*CELLW,y*CELLH,c->marks[k],c->fg,c->wide==2?2:1);
    }
    int graph_count=0;
    for(int i=0;i<state.image_count;i++){
        image *im=&state.images[i];
        CHECK(sample_hash(im->src,im->sw,im->sh)==im->hash);
        if(im->row==0&&im->cols==LJ_RIBBON_SPARK){
            CHECK(graph_count<3);graph_count++;
            for(int j=0;j<i;j++)CHECK(im->src!=state.images[j].src);
        }
        char *scratch=malloc(SIXEL_CAP);uint32_t *buf=NULL;size_t bufn=0;CHECK(scratch);
        FILE *wire=tmpfile();CHECK(wire);fflush(stdout);int saved=dup(STDOUT_FILENO);CHECK(saved>=0);
        CHECK(dup2(fileno(wire),STDOUT_FILENO)>=0);int emitted=emit_image(im,i,scratch,&buf,&bufn);
        CHECK(dup2(saved,STDOUT_FILENO)>=0);close(saved);CHECK(emitted>0);
        long size=ftell(wire);CHECK(size>0);rewind(wire);char *bytes=malloc((size_t)size);CHECK(bytes);
        CHECK(fread(bytes,1,(size_t)size,wire)==(size_t)size);fclose(wire);
        int w=im->cols*state.cellw,h=im->rows*state.cellh;uint32_t *pixels=calloc((size_t)w*h,4);CHECK(pixels);
        decode_sixel(bytes,(int)size,pixels,w,h);
        int highest=h;
        for(int y=0;y<h;y++)for(int x=0;x<w;x++){
            uint32_t c=pixels[y*w+x];lj_render_rect(im->col*CELLW+x,im->row*CELLH+y,1,1,c);
            if(c&&y<highest)highest=y;
        }
        if(im->row==0&&im->cols==LJ_RIBBON_SPARK)printf("ANSI emitted graph%d: height=%d ink_height=%d bytes=%ld stable_source PASS\n",graph_count,h,h-highest,size);
        free(pixels);free(bytes);free(scratch);free(buf);
    }
    if(state.sixel)CHECK(graph_count==3);
    if(state.sixel)for(int i=0;i<state.border_count;i++){
        pixel_border *b=&state.borders[i];if(!b->wire)continue;
        int w,h;uint32_t *pixels;
        if(b->separator){w=b->cols*CELLW;h=b->rows*CELLH;pixels=calloc((size_t)w*h,4);CHECK(pixels);decode_sixel(b->wire,(int)b->bytes,pixels,w,h);}
        else pixels=frame_raster(b,&w,&h);
        for(int y=0;y<h;y++)for(int x=0;x<w;x++)if(pixels[y*w+x])lj_render_rect(b->col*CELLW+x,b->row*CELLH+y,1,1,pixels[y*w+x]);
        free(pixels);
    }
    if(dir){char path[PATH_MAX];snprintf(path,sizeof(path),"%s/%s-%dx%d.png",dir,name,a->w,a->h);CHECK(!lj_render_save_png(path));}
}

static void sample_at(int t){
    float v=(float)((t/20)%20)/20.f;
    for(int m=0;m<LJ_METRIC_COUNT;m++)G.hist[m][G.head]=m==LJ_METRIC_MEM?.42f:v;
    G.head=(G.head+1)%LJ_METRIC_HISTORY;if(G.count<LJ_METRIC_HISTORY)G.count++;
    display_sample((uint64_t)t);
}
/* The plots draw the stacked PART rings (2026-09-13); planting the aggregate
 * alone would leave them at the real sampler's values, so plant both. */
static void plant_parts(int i,float gpu,float cpu,float mem){
    int cores=G.core_count;for(int c=0;c<cores;c++)G.part_display[PART_CPU+c][i]=cpu/(float)cores;
    G.part_display[PART_MEM][i]=mem*.6f;G.part_display[PART_MEM+1][i]=mem*.4f;G.part_display[PART_MEM+2][i]=0;
    G.part_display[PART_GPU][i]=gpu*.5f;G.part_display[PART_GPU+1][i]=gpu*.5f;
}
static uint64_t header_hash(App *a){
    uint32_t *pixels=lj_render_pixels();uint64_t hash=1469598103934665603ULL;
    for(int i=0;i<a->w*CELLH;i++){hash^=pixels[i];hash*=1099511628211ULL;}
    return hash;
}
static void shot(App *a,const char *dir,const char *name){
    if(!dir)return;char path[PATH_MAX];snprintf(path,sizeof(path),"%s/%s-%dx%d.png",dir,name,a->w,a->h);
    CHECK(lj_render_save_png(path)==0);
}
int main(void){
    /* This test exercises the bucket mechanism at 25 ms; the DEFAULT is 50 ms since 2026-09-13
     * (the operator: "1 px over 50 ms"), so pin the runtime knob here. */
    setenv("LJ_METRICS_DISPLAY_MS","25",1);

    CHECK(setlocale(LC_ALL,"")!=NULL);
    App *a=calloc(1,sizeof(*a));CHECK(a);test_app=a;
    a->w=800;a->h=560;a->running=1;a->demo=1;a->effects=1;a->backend_in=a->backend_out=-1;a->split=-1;
    for(int i=0;i<COUNT;i++)a->sessions[i].fd=-1;
    lj_dock_init(&a->dock);lj_media_init(&a->media);
    CHECK(SDL_Init(SDL_INIT_VIDEO)==0);CHECK(lj_render_init(a->w,a->h)==0);
    demo(a);a->ansi=0;a->mousex=-1;a->mousey=-1;
    memset(&G,0,sizeof(G));G.smooth_window=100;
    const char *dir=getenv("LILJACK_HEADER_CAPTURE_DIR");
    uint64_t prev=0;int changes=0;
    /* 5ms frames, 25ms samples, 25ms publication: the header may change only on a sample tick. */
    for(int t=0;t<=2000;t+=5){
        a->anim=(unsigned)t/120;
        if(t%25==0)sample_at(t);
        render(a);uint64_t hash=header_hash(a);
        if(t&&hash!=prev){CHECK(t%25==0);changes++;}
        prev=hash;
        printf("frame_ms=%d sample=%d label=%s header_hash=%llu\n",t,t%25==0,lj_metrics_display_label(LJ_METRIC_CPU),(unsigned long long)hash);
        if(t%100==0){char name[64];snprintf(name,sizeof(name),"sequence-%04dms",t);shot(a,dir,name);}
    }
    CHECK(changes>5&&changes<=80);
    for(int k=0;k<2;k++){
        a->w=k?1920:800;a->h=k?1080:560;CHECK(lj_render_init(a->w,a->h)==0);
        a->ribbon_open=0;a->ribbon=0;menu_close(a);render(a);shot(a,dir,"closed");
        uint64_t closed_hash=header_hash(a);a->mousex=10;a->mousey=10;render(a);
        CHECK(header_hash(a)!=closed_hash);shot(a,dir,"logo-hover");a->mousex=-1;a->mousey=-1;render(a);
        a->ribbon_open=1;a->ribbon=RIBBON_FULL;render(a);shot(a,dir,"open");
        if(!k){click_hit(H_RIBBON_MORE,NULL);render(a);CHECK(a->ribbon_open);
            for(int frame=0;frame<10;frame++)render(a);CHECK(a->ribbon==RIBBON_FULL);shot(a,dir,"more");menu_close(a);}
        /* Every action follows a real hit, including overflow and QUIT. */
        MenuRow rows[16];int n=toolbar_rows(a,rows,16);
        for(int i=0;i<n;i++){
            menu_close(a);click_toolbar(rows[i].action);
            if(rows[i].action==H_QUIT){CHECK(!a->running);a->running=1;}
            printf("toolbar width=%d action=%s reached by mouse\n",a->w,rows[i].label);
        }
        a->about_open=0;a->full[0]=0;a->ribbon_open=0;a->ribbon=0;menu_close(a);
    }
    /* Real sampler warmup, then ANSI and deferred sixel, not pixel-only render. */
    a->w=800;a->h=560;a->ansi=1;a->ribbon_open=0;a->ribbon=0;
    setenv("LJ_METRICS_NVML","/nonexistent/liljack-test-nvml",1);
    CHECK(lj_metrics_init()==0);
    double began=monotonic();
    while(monotonic()-began<1.2){lj_metrics_poll();struct timespec pause={0,1000000};nanosleep(&pause,NULL);}
    CHECK(G.display_count>=10);
    state.opened=1;state.cellw=10;state.cellh=20;state.sixel=0;
    render(a);ansi_capture(a,dir,"ansi-real-1200ms");
    /* Plant distinct completed values after acquisition to test 25/75/50% heights. */
    for(int i=0;i<G.display_count;i++){
        G.display[LJ_METRIC_GPU][i]=.75f;G.display[LJ_METRIC_CPU][i]=.25f;G.display[LJ_METRIC_MEM][i]=.5f;
        plant_parts(i,.75f,.25f,.5f);
    }
    G.paced_started=0;   /* planted straight into the display ring: force the paced snapshot to pick it up */
    render(a);ansi_capture(a,dir,"ansi-planted-1200ms");
    state.sixel=1;render(a);ansi_capture(a,dir,"sixel-planted-1200ms");
    /* ONE PIXEL COLUMN PER BUCKET (the operator 2026-09-12): plant alternating 25/75%
     * buckets and require ADJACENT pixel columns of every header plot to differ
     * in ink height. A cell-stretched plot (the old bug) fails this on 9 of
     * every 10 column pairs. */
    for(int i=0;i<G.display_count;i++){float v=i&1?.75f:.25f;
        G.display[LJ_METRIC_GPU][i]=v;G.display[LJ_METRIC_CPU][i]=v;G.display[LJ_METRIC_MEM][i]=v;plant_parts(i,v,v,v);}
    G.paced_started=0;
    render(a);ansi_capture(a,dir,"sixel-1px-alternating");
    {
        int pw=LJ_RIBBON_SPARK*CELLW,ph=CELLH>40?40:CELLH,n=G.display_count;
        int lit=n<pw?n:pw;CHECK(lit>=10);
        for(int m=0;m<3;m++){
            int pairs=0;
            for(int c=pw-lit;c+1<pw;c++){
                int h0=0,h1=0;for(int y=0;y<ph;y++){h0+=a->header_plot[m][y*pw+c]>>24?1:0;h1+=a->header_plot[m][y*pw+c+1]>>24?1:0;}
                CHECK(h0>0&&h1>0&&h0!=h1);pairs++;
            }
            for(int c=0;c<pw-lit;c++)for(int y=0;y<ph;y++)CHECK(!(a->header_plot[m][y*pw+c]>>24)); /* no history: no ink, never a 0% lie */
            printf("Header plot %d: %d adjacent 1px columns all differ (25ms per column) PASS\n",m,pairs);
        }
        /* STACKED COLUMNS (the operator 2026-09-13): plant the first k cores at 20% each
         * (the rest idle) and require a CPU column's pixels, bottom to top, to run
         * through the core colours IN CORE ORDER, the stack top at the aggregate
         * height. Then take the GPU away (NaN, as NVML absent gives) and require
         * the GPU plot to go dark while CPU and MEM keep their stacks. */
        int cores=G.core_count,k=cores<4?cores:4;CHECK(k>=1);
        for(int i=0;i<n;i++){for(int c=0;c<cores;c++)G.part_display[PART_CPU+c][i]=c<k?.2f:0.f;G.display[LJ_METRIC_CPU][i]=.2f*k;}
        G.paced_started=0;render(a);
        {
            int c=pw-1,seen=0;uint32_t prev=0;
            for(int y=ph-1;y>=0;y--){uint32_t px=a->header_plot[1][y*pw+c];if(!(px>>24))break;
                if((px&0xffffff)!=prev){CHECK(seen<k);CHECK((px&0xffffff)==core_rgb(seen,cores));prev=px&0xffffff;seen++;}}
            int top=0;for(int y=0;y<ph;y++)top+=a->header_plot[1][y*pw+c]>>24?1:0;
            CHECK(seen==k&&top==(int)(.2f*k*(ph-1)+0.5f));
            printf("Header CPU stack: %d core segments in core order, top at %dpx for %.0f%% PASS\n",seen,top,20.f*k);
        }
        for(int i=0;i<n;i++){G.display[LJ_METRIC_GPU][i]=NAN;G.part_display[PART_GPU][i]=NAN;G.part_display[PART_GPU+1][i]=NAN;}   /* NVML gone: aggregate and parts */
        G.paced_started=0;render(a);
        {
            int ink[3]={0,0,0};for(int m=0;m<3;m++)for(int i=0;i<pw*ph;i++)ink[m]+=a->header_plot[m][i]>>24?1:0;
            CHECK(ink[0]==0&&ink[1]>0&&ink[2]>0);
            printf("Header GPU unavailable: GPU plot empty, CPU ink=%d MEM ink=%d PASS\n",ink[1],ink[2]);
        }
    }
    state.opened=0;a->ansi=0;lj_metrics_close();
    test_cleanup();puts("Header: 2s pixel stability at 10ms frames/25ms samples; 25ms publication, 1px per bucket; all nine mouse routes at800/1920 PASS");
}
