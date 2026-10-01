/* Real controller and terminal canvas. The Python runner owns the PTY and
 * supplies either a local fixture or the built-in public video source. */
static const char *test_video;
#define LJ_RICKROLL test_video
#define main liljack_entry
#include "../liljack_app/main.c"
#undef main
#include "../liljack_app/c_ansi.c"

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c); exit(1); } } while (0)
static App *app;
static void cleanup_test(void){
    if(app){lj_popup_close(&app->popup);lj_media_close(&app->media);free(app);}
    lj_ansi_close();lj_render_close();
}
static void mouse(int type,int x,int y){
    SDL_Event e={0};e.type=type;
    if(type==SDL_MOUSEMOTION){e.motion.x=x;e.motion.y=y;e.motion.state=SDL_BUTTON_LMASK;}
    else {e.button.x=x;e.button.y=y;e.button.button=SDL_BUTTON_LEFT;}
    event(app,&e);
}
static void click(int kind){
    for(int i=0;i<app->nhits;i++)if(app->hits[i].kind==kind){
        lj_rect r=app->hits[i].r;mouse(SDL_MOUSEBUTTONDOWN,r.x+r.w/2,r.y+r.h/2);
        mouse(SDL_MOUSEBUTTONUP,r.x+r.w/2,r.y+r.h/2);return;
    }
    CHECK(!"missing click target");
}
/* ⚠ THE NINE LEFT THE HEADER (thin-HUI §2, tui-burger-ribbon). ABOUT is no
 * longer a badge in the tab row; it is an item in the burger ribbon and its hit
 * exists only while the ribbon is out. This reaches it the way a user does —
 * open the ribbon, click the item — the same helper the native suite gained
 * when its click_hit(H_REVIEW) sites broke for the same reason. This harness
 * was missed then and failed on "missing click target" at click(H_ABOUT). */
static void click_toolbar(int action){
    app->ribbon_open=1;app->ribbon=RIBBON_FULL;render(app);
    MenuRow rows[16];int n=toolbar_rows(app,rows,16),idx=-1;
    for(int k=0;k<n;k++)if(rows[k].action==action)idx=k;
    CHECK(idx>=0);
    for(int i=0;i<app->nhits;i++)if(app->hits[i].kind==H_RIBBON_ITEM&&app->hits[i].index==idx){
        lj_rect r=app->hits[i].r;mouse(SDL_MOUSEBUTTONDOWN,r.x+r.w/2,r.y+r.h/2);
        mouse(SDL_MOUSEBUTTONUP,r.x+r.w/2,r.y+r.h/2);return;}
    CHECK(!"missing ribbon item");
}
static void frame(void){render(app);CHECK(lj_ansi_present());}
static void first_frame(void){
    Uint32 until=SDL_GetTicks()+25000;
    while(!app->popup.media.pixels&&SDL_GetTicks()<until){popup_poll(app);SDL_Delay(5);}
    CHECK(app->popup.media.pixels);CHECK(app->popup.media.w==640);frame();
}
static void canvas_png(const char *path){
    /* Reconstruct the very canvas/queued image regions passed to present().
     * This is a terminal-output reconstruction, not a desktop screenshot. */
    for(int y=0;y<state.rows;y++)for(int x=0;x<state.cols;x++){
        cell *c=&state.canvas[y*state.cols+x];
        lj_render_rect(x*CELLW,y*CELLH,CELLW,CELLH,c->bg);
        if(c->cp==0x2580)lj_render_rect(x*CELLW,y*CELLH,CELLW,CELLH/2,c->fg);
        else if(c->cp&&c->cp!=' ')lj_render_glyph(x*CELLW,y*CELLH,c->cp,c->fg,c->wide==2?2:1);
    }
    for(int i=0;i<state.image_count;i++){
        image *im=&state.images[i];lj_media m={.pixels=(uint32_t*)im->src,.w=im->sw,.h=im->sh};
        lj_media_blit(&m,lj_render_pixels(),app->w,app->h,im->col*CELLW,im->row*CELLH,im->cols*CELLW,im->rows*CELLH);
    }
    CHECK(lj_render_save_png(path)==0);
}
int main(int argc,char **argv){
    CHECK(argc==3);test_video=argv[1];setlocale(LC_CTYPE,"");signal(SIGPIPE,SIG_IGN);
    CHECK(atexit(cleanup_test)==0);app=calloc(1,sizeof(*app));CHECK(app);
    App *a=app;a->ansi=1;a->demo=1;a->running=1;a->split=-1;
    a->backend_in=a->backend_out=-1;for(int i=0;i<COUNT;i++)a->sessions[i].fd=-1;
    lj_dock_init(&a->dock);lj_media_init(&a->media);lj_popup_init(&a->popup);
    CHECK(lj_ansi_open(&a->w,&a->h));CHECK(lj_render_init(a->w,a->h)==0);
    copy(a->focus,sizeof(a->focus),"room");lj_dock_drop(&a->dock,"room",NULL,LJ_DOCK_LEFT);
    frame();int before=a->dock.tile_count;
    /* ⚠ THE HEADER NOW OWNS SIXEL IMAGES OF ITS OWN (thin-HUI §1: the mark, and
     * the load plots when there is data). The claim this harness makes is about
     * the POPUP — it adds exactly one image and takes it away on close — so the
     * header's count is measured here, with no popup open, and used as the
     * baseline instead of assuming the video is the only picture on screen. */
    int hdr_images=state.sixel?state.image_count:0;
    /* Actual click path, including the About hit list and mouse-up event. */
    click_toolbar(H_ABOUT);frame();CHECK(a->about_open);click(H_ABOUT_PLAY);
    CHECK(lj_popup_is_open(&a->popup));CHECK(!a->about_open);CHECK(!a->has_media);
    first_frame();CHECK(a->dock.tile_count==before);CHECK(!dock_has(a,"media"));
    if(!state.sixel){int blocks=0;for(int i=0;i<state.cols*state.rows;i++)blocks+=state.canvas[i].cp==0x2580;CHECK(blocks>100);}
    else CHECK(state.image_count==hdr_images+1&&state.shown_cols[hdr_images]>0);
    canvas_png(argv[2]);
    click(H_POPUP_TOGGLE);CHECK(a->popup.media.paused);frame();
    double paused=lj_media_position(&a->popup.media);
    SDL_Delay(100);popup_poll(a);CHECK(fabs(lj_media_position(&a->popup.media)-paused)<.04);
    click(H_POPUP_MUTE);CHECK(a->popup.media.muted);
    click(H_POPUP_VOL_DOWN);CHECK(a->popup.media.volume==60);
    click(H_POPUP_VOL_UP);CHECK(a->popup.media.volume==70);
    CHECK(a->popup.media.duration>0);click(H_POPUP_SEEK);
    CHECK(fabs(a->popup.media.offset-a->popup.media.duration/2)<.5);
    first_frame();CHECK(a->popup.media.paused);CHECK(a->popup.media.muted);
    click(H_POPUP_BACK);CHECK(a->popup.media.offset>=0);first_frame();
    double previous=a->popup.media.offset;
    click(H_POPUP_FORWARD);CHECK(a->popup.media.offset>previous);
    click(H_POPUP_SEEK);first_frame();
    click(H_POPUP_MUTE);CHECK(!a->popup.media.muted);
    click(H_POPUP_TOGGLE);CHECK(!a->popup.media.paused);frame();
    lj_rect old=a->popup_rect;
    mouse(SDL_MOUSEBUTTONDOWN,old.x+2*CELLW,old.y+CELLH/2);
    mouse(SDL_MOUSEMOTION,-100,-100);mouse(SDL_MOUSEBUTTONUP,-100,-100);frame();
    CHECK(a->popup_rect.x==0&&a->popup_rect.y==0);CHECK(!a->popup_dragging);
    SDL_Event resize={.type=SDL_WINDOWEVENT};resize.window.event=SDL_WINDOWEVENT_SIZE_CHANGED;
    resize.window.data1=800;resize.window.data2=560;event(a,&resize);frame();
    CHECK(a->popup_rect.x+a->popup_rect.w<=a->w);CHECK(a->popup_rect.y+a->popup_rect.h<=a->h);
    pid_t decoder=a->popup.media.pid;int fd=a->popup.media.fd;
    click(H_POPUP_CLOSE);popup_poll(a);frame();CHECK(!lj_popup_is_open(&a->popup));
    CHECK(a->popup.media.fd==-1&&a->popup.media.pid==0);
    if(decoder>0){errno=0;CHECK(waitpid(decoder,NULL,WNOHANG)==-1&&errno==ECHILD);}
    if(fd>=0){errno=0;CHECK(fcntl(fd,F_GETFD)==-1&&errno==EBADF);}
    CHECK(state.image_count==hdr_images);for(int i=hdr_images;i<MAX_IMAGES;i++)CHECK(state.shown_cols[i]==0);
    /* Reopen and close with Escape, before any decoded frame is required. */
    a->about_open=1;frame();click(H_ABOUT_PLAY);CHECK(lj_popup_is_open(&a->popup));
    SDL_Event esc={.type=SDL_KEYDOWN};esc.key.keysym.sym=SDLK_ESCAPE;event(a,&esc);popup_poll(a);
    CHECK(!lj_popup_is_open(&a->popup));CHECK(!a->popup_dragging);
    /* Existing --media behavior remains docked and survives popup close. */
    CHECK(lj_dock_valid(&a->dock));
    open_media(a,test_video);CHECK(a->has_media);
    CHECK(dock_has(a,"media"));frame();CHECK(dock_has(a,"media"));
    lj_media_close(&a->media);a->has_media=0;
    /* Overlay chrome must cover queued lower sixel images, not vice versa. */
    if(state.sixel){
        uint32_t blue[4]={0xff0000ff,0xff0000ff,0xff0000ff,0xff0000ff};
        lj_ansi_begin(a->w,a->h);CHECK(lj_ansi_image(0,0,100,100,blue,2,2));
        CHECK(state.image_count==1);lj_ansi_cover(20,20,20,20);CHECK(state.image_count==0);
        CHECK(state.canvas[0].cp==0x2580);lj_ansi_rect(20,20,20,20,0xff0000);
        CHECK(state.canvas[state.cols+2].bg==0xff0000);CHECK(lj_ansi_present());
    }
    fprintf(stderr,"PASS popup: About click, first frame, terminal pixels, pause/timeline/seek/mute/volume clicks, drag/clamp, resize, close/reap, reopen/Escape, docked media\n");
    return 0;
}
