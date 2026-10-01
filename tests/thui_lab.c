/* Selectable, isolated THUI combinations using the production controller. */
#define main liljack_application_main
#include "../liljack_app/main.c"
#undef main
#include "../liljack_app/c_ansi.c"
#include <assert.h>
#include <sys/resource.h>

static void lab_pointer(App *a, Uint32 type, int x, int y) {
    SDL_Event e = {0}; e.type = type;
    if (type == SDL_MOUSEMOTION) { e.motion.x=x; e.motion.y=y; }
    else { e.button.button=SDL_BUTTON_LEFT; e.button.x=x; e.button.y=y; }
    event(a,&e);
}
static void lab_png(App *a, const char *path) {
    /* Headless evidence is explicitly the cell fallback, never sixel proof. */
    assert(state.image_count==0);
    lj_render_resize(a->w,a->h);
    for(int y=0;y<state.rows;y++)for(int x=0;x<state.cols;x++) {
        cell *c=&state.canvas[y*state.cols+x];
        lj_render_rect(x*CELLW,y*CELLH,CELLW,CELLH,c->bg);
    }
    for(int y=0;y<state.rows;y++)for(int x=0;x<state.cols;x++) {
        cell *c=&state.canvas[y*state.cols+x];
        if(c->cp==0x2580)lj_render_rect(x*CELLW,y*CELLH,CELLW,CELLH/2,c->fg);
        else if(c->cp&&c->cp!=' ')lj_render_glyph(x*CELLW,y*CELLH,c->cp,c->fg,c->wide==2?2:1);
        for(int k=0;k<3&&c->marks[k];k++)lj_render_glyph(x*CELLW,y*CELLH,c->marks[k],c->fg,c->wide==2?2:1);
    }
    assert(lj_render_save_png(path)==0);
}
static void lab_capture(App *a,const char *dir,const char *name) {
    char path[PATH_MAX];snprintf(path,sizeof path,"%s/%s.png",dir,name);lab_png(a,path);
}
static void lab_geometry(App *a) {
    assert(lj_dock_valid(&a->dock));
    for(int i=0;i<a->dock.tile_count;i++) {
        lj_rect r=a->dock.tiles[i].rect;
        assert(r.x>=0&&r.y>=0&&r.w>0&&r.h>0&&r.x+r.w<=a->w&&r.y+r.h<=a->h);
        assert(r.x%CELLW==0&&r.y%CELLH==0&&r.w%CELLW==0&&r.h%CELLH==0);
        for(int j=0;j<i;j++) {
            lj_rect s=a->dock.tiles[j].rect;
            assert(!(r.x<s.x+s.w&&s.x<r.x+r.w&&r.y<s.y+s.h&&s.y<r.y+r.h));
        }
    }
}
static double lab_ms(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000.0+t.tv_nsec/1e6;
}
static int lab_cmp(const void *a,const void *b) {
    double x=*(const double*)a,y=*(const double*)b;return (x>y)-(x<y);
}
int main(int argc,char **argv) {
    if(argc!=7){fprintf(stderr,"case width height frames output-dir interactive(0|1)\n");return 2;}
    const char *which=argv[1],*dir=argv[5];int interactive=atoi(argv[6]);
    setlocale(LC_CTYPE,"C.UTF-8");signal(SIGPIPE,SIG_IGN);
    App *a=calloc(1,sizeof *a);assert(a);
    a->w=atoi(argv[2]);a->h=atoi(argv[3]);a->demo=a->ansi=a->running=1;
    a->split=-1;a->backend_in=a->backend_out=-1;a->mousex=a->mousey=-1;
    for(int i=0;i<COUNT;i++)a->sessions[i].fd=-1;
    lj_dock_init(&a->dock);lj_media_init(&a->media);lj_popup_init(&a->popup);
    if(interactive&&!lj_ansi_open(&a->w,&a->h)){free(a);return 1;}
    assert(!lj_render_init(a->w,a->h));demo(a);a->toast[0]=0;
    /* Two terminal tiles plus the room: smallest useful split interaction. */
    const char *ids[]={"demo-0","demo-1"};assert(lj_dock_retile(&a->dock,ids,2));
    if(!strcmp(which,"dialog")) {
        copy(a->project,sizeof a->project,"/tmp/THUI-lab");room_dialog_open(a);
    } else if(!strcmp(which,"popup")) {
        a->popup.open=1;copy(a->popup.status,sizeof a->popup.status,"THUI lab: synthetic unavailable-media state");
    } else assert(!strcmp(which,"tiles"));
    render(a);lab_geometry(a);
    if(interactive) {
        /* Real terminal event/present path; demo prevents agent/backend launch. */
        while(a->running&&!quitting) {
            SDL_Event e;
            while(lj_ansi_poll(&e)) {
                if(e.type==SDL_KEYDOWN&&e.key.keysym.sym==SDLK_F10)a->running=0;
                else event(a,&e);
            }
            if(!a->running)break;
            render(a);if(!lj_ansi_present())break;SDL_Delay(33);
        }
    } else {
        lab_capture(a,dir,"default");
        int x=0,y=0;
        if(!strcmp(which,"tiles")) {
            assert(a->dock.divider_count);lj_rect r=hitrect(a->dock.dividers[0].rect);x=r.x+r.w/2;y=r.y+r.h/2;
        } else {
            int kind=!strcmp(which,"dialog")?H_ROOM_FIELD:H_POPUP_MUTE,found=0;
            for(int i=0;i<a->nhits;i++)if(a->hits[i].kind==kind&&
                (kind!=H_ROOM_FIELD||a->hits[i].index==RF_FOLDER)){
                x=a->hits[i].r.x+CELLW;y=a->hits[i].r.y+CELLH/2;found=1;break;}
            assert(found);
        }
        int muted=a->popup.media.muted;lj_rect first=a->dock.tiles[0].rect;
        lab_pointer(a,SDL_MOUSEMOTION,x,y);render(a);lab_capture(a,dir,"hover");
        lab_pointer(a,SDL_MOUSEBUTTONDOWN,x,y);render(a);lab_capture(a,dir,"pressed");
        if(!strcmp(which,"tiles"))lab_pointer(a,SDL_MOUSEMOTION,x+40,y+40);
        lab_pointer(a,SDL_MOUSEBUTTONUP,x+(!strcmp(which,"tiles")?40:0),y+(!strcmp(which,"tiles")?40:0));
        render(a);lab_geometry(a);lab_capture(a,dir,"after");
        if(!strcmp(which,"dialog"))assert(a->room_field==RF_FOLDER);
        if(!strcmp(which,"popup"))assert(a->popup.media.muted!=muted);
        if(!strcmp(which,"tiles"))assert(memcmp(&first,&a->dock.tiles[0].rect,sizeof first));
        /* Exercise reset/open/close and viewport changes, not just paint. */
        lj_dock saved=a->dock;int width=a->w,height=a->h;
        for(int i=0;i<100;i++) {
            a->w=i%2?width:800;a->h=i%2?height:560;
            if(!strcmp(which,"dialog")){a->room_dialog=0;render(a);room_dialog_open(a);}
            if(!strcmp(which,"popup")){a->popup.open=0;render(a);a->popup.open=1;}
            a->dock=saved;render(a);lab_geometry(a);
        }
        a->w=width;a->h=height;render(a);
        int n=clamp(atoi(argv[4]),10,100000);double *times=calloc(n,sizeof *times);assert(times);
        for(int i=0;i<20;i++)render(a); /* warm font/cache paths */
        for(int i=0;i<n;i++) {
            lab_pointer(a,SDL_MOUSEMOTION,i%2?x:-1,i%2?y:-1);
            double start=lab_ms();render(a);times[i]=lab_ms()-start;lab_geometry(a);
        }
        qsort(times,n,sizeof *times,lab_cmp);struct rusage usage;getrusage(RUSAGE_SELF,&usage);
        printf("{\"case\":\"%s\",\"width\":%d,\"height\":%d,\"frames\":%d,\"render_ms_p50\":%.4f,\"render_ms_p95\":%.4f,\"render_ms_max\":%.4f,\"peak_rss_kib\":%ld,\"geometry\":\"pass\"}\n",which,a->w,a->h,n,times[n/2],times[(n*95)/100],times[n-1],usage.ru_maxrss);
        free(times);
    }
    for(int i=0;i<a->nsession;i++)if(a->sessions[i].vt)lj_vt_free(a->sessions[i].vt);
    if(a->messages)json_object_put(a->messages);drafts_close(a);lj_popup_close(&a->popup);lj_media_close(&a->media);
    free(a->pending);free(a);lj_render_close();lj_ansi_close();SDL_Quit();return 0;
}
