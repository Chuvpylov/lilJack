#define main liljack_entry
#include "../liljack_app/main.c"
#undef main
#include "../liljack_app/c_ansi.c"
#include <assert.h>

static void pointer(App *a,int type,int x,int y){
    SDL_Event e={0};e.type=type;
    if(type==SDL_MOUSEMOTION){e.motion.x=x;e.motion.y=y;}
    else{e.button.x=x;e.button.y=y;e.button.button=SDL_BUTTON_LEFT;}
    assert(popup_event(a,&e));
}
static void draw(App *a){render_ansi=1;a->nhits=0;lj_ansi_begin(a->w,a->h);popup_render(a);}
static void click(App *a,int kind){
    draw(a);
    for(int i=0;i<a->nhits;i++)if(a->hits[i].kind==kind){
        lj_rect r=a->hits[i].r;int x=r.x+r.w/2,y=r.y+r.h/2;
        pointer(a,SDL_MOUSEBUTTONDOWN,x,y);pointer(a,SDL_MOUSEBUTTONUP,x,y);return;
    }
    assert(!"missing popup control");
}
static void decoded(App *a){
    Uint32 until=SDL_GetTicks()+10000;
    while(!a->popup.media.pixels&&!a->popup.media.ended&&SDL_GetTicks()<until){popup_poll(a);SDL_Delay(2);}
    assert(a->popup.media.pixels);
}
static void capture(App *a,const char *dir,const char *tag){
    if(!dir)return;
    draw(a);lj_render_resize(a->w,a->h);
    int content=0;for(int i=0;i<state.rows*state.cols;i++)content+=state.canvas[i].content!=0;
    assert(content>10);
    for(int y=0;y<state.rows;y++)for(int x=0;x<state.cols;x++){
        cell *c=&state.canvas[y*state.cols+x];
        lj_render_rect(x*CELLW,y*CELLH,CELLW,CELLH,c->bg);
        if(c->cp==0x2580)lj_render_rect(x*CELLW,y*CELLH,CELLW,CELLH/2,c->fg);
        else if(c->cp&&c->cp!=' ')lj_render_glyph(x*CELLW,y*CELLH,c->cp,c->fg,c->wide==2?2:1);
    }
    char path[1024];snprintf(path,sizeof(path),"%s/%s-%dx%d.png",dir,tag,a->w,a->h);
    assert(!lj_render_save_png(path));
}
int main(int argc,char **argv){
    assert(argc>=2);setlocale(LC_CTYPE,"C.UTF-8");signal(SIGPIPE,SIG_IGN);
    assert(!lj_render_init(800,560));
    state.opened=1;state.cellw=CELLW;state.cellh=CELLH; /* isolated presenter, no host terminal */
    App *a=calloc(1,sizeof(*a));assert(a);
    a->ansi=1;a->backend_in=a->backend_out=-1;
    lj_popup_init(&a->popup);a->popup.open=1;a->w=800;a->h=560;
    popup_geometry(a);lj_rect r=a->popup_rect;
    int x=r.x+r.w-1,y=r.y+r.h-1;
    pointer(a,SDL_MOUSEBUTTONDOWN,x,y);
    pointer(a,SDL_MOUSEMOTION,x-100,y-80);
    pointer(a,SDL_MOUSEBUTTONUP,x-100,y-80);
    popup_geometry(a);
    assert(a->popup_rect.w==r.w-100&&a->popup_rect.h==r.h-80);
    puts("PASS persistent popup corner resize (failed before fix)");
    int sizes[][2]={{800,560},{1280,720},{1920,1080}};
    for(int s=0;s<3;s++){
        a->w=sizes[s][0];a->h=sizes[s][1];a->popup_placed=0;popup_geometry(a);r=a->popup_rect;
        capture(a,argc>2?argv[2]:NULL,"default");
        x=r.x+r.w-1;y=r.y+r.h-1;
        pointer(a,SDL_MOUSEBUTTONDOWN,x,y);pointer(a,SDL_MOUSEMOTION,-1000,-1000);
        assert(a->popup_rect.w==20*CELLW&&a->popup_rect.h==7*CELLH);
        capture(a,argc>2?argv[2]:NULL,"minimum-drag");
        pointer(a,SDL_MOUSEBUTTONUP,-1000,-1000);assert(!a->popup_resizing);
        draw(a);
        int controls=0;for(int i=0;i<a->nhits;i++){
            Hit *h=&a->hits[i];
            if(h->kind>=H_POPUP_TOGGLE&&h->kind<=H_POPUP_VOL_UP)controls++;
            assert(h->r.x>=r.x&&h->r.y>=r.y&&h->r.x+h->r.w<=r.x+a->popup_rect.w);
        }assert(controls==6);
        r=a->popup_rect;x=r.x+r.w-1;y=r.y+r.h-1;
        pointer(a,SDL_MOUSEBUTTONDOWN,x,y);pointer(a,SDL_MOUSEMOTION,10000,10000);
        pointer(a,SDL_MOUSEBUTTONUP,10000,10000);
        assert(a->popup_rect.x+a->popup_rect.w<=a->w&&a->popup_rect.y+a->popup_rect.h<=a->h);
        r=a->popup_rect;popup_geometry(a);assert(!memcmp(&r,&a->popup_rect,sizeof(r)));
        capture(a,argc>2?argv[2]:NULL,"expanded");
        pointer(a,SDL_MOUSEBUTTONDOWN,r.x+CELLW,r.y+CELLH/2);
        pointer(a,SDL_MOUSEMOTION,-100,-100);pointer(a,SDL_MOUSEBUTTONUP,-100,-100);
        assert(a->popup_rect.x==0&&a->popup_rect.y==0&&a->popup_rect.w==r.w);
    }
    /* Real decoder: red before two seconds, blue after; size changes must seek. */
    assert(lj_popup_open(&a->popup,argv[1]));
    assert(lj_media_resolution(&a->popup.media,480)); /* while ffprobe is pending */
    lj_media_pause(&a->popup.media,1);decoded(a);
    lj_media *m=&a->popup.media;assert(m->w==854&&m->h==480&&m->has_audio&&m->audio_device);
    assert(lj_popup_seek(&a->popup,3));decoded(a);
    lj_media_mute(m,1);lj_media_volume(m,30);
    int heights[]={720,360,480};
    for(int i=0;i<3;i++){
        double before=lj_media_position(m);pid_t pid=m->pid;
        click(a,H_POPUP_RESOLUTION);assert(m->decode_h==heights[i]);decoded(a);
        assert(m->h==heights[i]&&m->w==(heights[i]==720?1280:heights[i]==480?854:640));
        assert(m->paused&&m->muted&&m->volume==30&&fabs(lj_media_position(m)-before)<.05);
        assert((m->pixels[(size_t)(m->h/2)*m->w+m->w/2]&255)>220);
        if(pid>0){errno=0;assert(waitpid(pid,NULL,WNOHANG)==-1&&errno==ECHILD);}
        draw(a);assert(a->popup_pixel_cap>=(size_t)m->w*m->h);
        state.sixel=1;draw(a);assert(state.image_count==1);
        assert(state.images[0].sw==m->w&&state.images[0].sh==m->h);state.sixel=0;
        char tag[32];snprintf(tag,sizeof(tag),"decoded-%dp",heights[i]);
        for(int s=0;s<3;s++){
            a->w=sizes[s][0];a->h=sizes[s][1];a->popup_placed=0;
            draw(a);int blue=0;for(int k=0;k<state.rows*state.cols;k++)
                blue+=state.canvas[k].cp==0x2580&&(state.canvas[k].fg&255)>220;
            assert(blue>100);capture(a,argc>2?argv[2]:NULL,tag);
        }
    }
    assert(!lj_media_resolution(m,999));
    lj_media_pause(m,0);double before=lj_media_position(m);
    click(a,H_POPUP_RESOLUTION);decoded(a);assert(!m->paused&&fabs(m->offset-before)<.1);
    lj_popup_request_close(&a->popup);popup_poll(a);
    assert(!a->popup_pixels&&!a->popup.open&&!a->popup_resizing&&m->pid==0);
    free(a);state.opened=0;lj_ansi_close();lj_render_close();
    puts("PASS 3 sizes: min/max resize, persistence, controls, drag; real 360/480/720 decode, position/color, pause/audio preferences, reaping and cleanup");
}
