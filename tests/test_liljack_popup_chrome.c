/* popup-chrome — the media popup on the cell grid (visual-polish-chrome item 4).
 * Opens the popup with a synthetic decoded frame (no ffmpeg, no network) and
 * renders the ANSI cell canvas at 800x560 / 1280x720 / 1920x1080. Asserts:
 *   C1 the outer ring of popup cells (the frame) carries NO text glyph —
 *      a 1 px strip owns its cell; a title in row 0 is the old chrome;
 *   C2 the title row shows the media TITLE, not a fixed banner;
 *   C3 the close hit is the last inner cell of the title row, the resize hit is
 *      the corner frame cell, the seek hit sits on row h-3 inside the frame;
 *   C4 the popup is never shorter than 10 rows (frame·title·rule·video·rule·
 *      timeline·controls·frame need it);
 *   C5 no cell inside the popup carries a chopped label (every text run inside
 *      ends before the right frame column). */
#define main liljack_entry
#include "../liljack_app/main.c"
#undef main
#include "../liljack_app/c_ansi.c"
#include <assert.h>
#include <locale.h>
static int checks,failed;
#define CHECK(c,...) do{checks++;if(!(c)){failed++;fprintf(stderr,"FAIL %s:%d ",__FILE__,__LINE__);fprintf(stderr,__VA_ARGS__);fputc('\n',stderr);}}while(0)
static uint32_t frame_px[640*360];
static const cell *at(int col,int row){return &state.canvas[row*state.cols+col];}
/* Box-drawing (U+2500..257F) and block elements (U+2580..259F) are what c_ansi
 * substitutes for a strip on a terminal without sixel: not text. */
static int is_line_glyph(uint32_t cp){return cp>=0x2500&&cp<=0x259F;}
static int has_text(int col,int row){uint32_t cp=at(col,row)->cp;return cp&&cp!=' '&&!is_line_glyph(cp);}
static Hit *find_hit(App *a,int kind){for(int i=a->nhits-1;i>=0;i--)if(a->hits[i].kind==kind)return &a->hits[i];return NULL;}
int main(void){
    setlocale(LC_CTYPE,"C.UTF-8");assert(!lj_render_init(1920,1080));
    for(int i=0;i<640*360;i++)frame_px[i]=0xff203050u+(uint32_t)(i%97);
    int sizes[3][2]={{800,560},{1280,720},{1920,1080}};
    for(int s=0;s<3;s++){
        App *a=calloc(1,sizeof *a);assert(a);a->w=sizes[s][0];a->h=sizes[s][1];a->ansi=1;a->demo=1;a->split=-1;a->backend_in=a->backend_out=-1;
        for(int i=0;i<COUNT;i++)a->sessions[i].fd=-1;
        lj_render_resize(a->w,a->h);lj_dock_init(&a->dock);lj_media_init(&a->media);lj_popup_init(&a->popup);demo(a);
        lj_popup *p=&a->popup;p->open=1;p->media.pixels=frame_px;p->media.w=640;p->media.h=360;p->media.is_video=1;
        p->media.duration=212;p->media.clock_base=42;p->media.has_audio=1;p->media.volume=70;
        snprintf(p->media.title,sizeof p->media.title,"%s","Never Gonna Give You Up");snprintf(p->status,sizeof p->status,"Playing");
        a->popup_placed=0;
        lj_ansi_begin(a->w,a->h);render(a);
        lj_rect r=a->popup_rect;int c0=r.x/CELLW,r0=r.y/CELLH,wc=r.w/CELLW,hc=r.h/CELLH;
        /* C4 */
        CHECK(hc>=10,"%dx%d popup is %d rows",a->w,a->h,hc);
        /* C1 frame ring text-free */
        int ring_text=0;
        for(int c=c0;c<c0+wc;c++){ring_text+=has_text(c,r0);ring_text+=has_text(c,r0+hc-1);}
        for(int rr=r0+1;rr<r0+hc-1;rr++){ring_text+=has_text(c0,rr);ring_text+=has_text(c0+wc-1,rr);}
        /* the corner frame cell is the resize GRIP: a control glyph (theme
         * token resize), the one non-line mark the ring may carry */
        if(has_text(c0+wc-1,r0+hc-1)&&at(c0+wc-1,r0+hc-1)->cp==lj_theme_codepoint(LJ_THEME_RESIZE))ring_text--;
        CHECK(ring_text==0,"%dx%d frame ring carries %d text cells (row0 starts '%lc%lc%lc')",a->w,a->h,ring_text,(wint_t)at(c0+1,r0)->cp,(wint_t)at(c0+2,r0)->cp,(wint_t)at(c0+3,r0)->cp);
        /* C2 title in row 1 */
        char row1[512]={0};size_t n=0;for(int c=c0+1;c<c0+wc-1&&n<500;c++){uint32_t cp=at(c,r0+1)->cp;if(cp<128&&cp)row1[n++]=(char)cp;else if(cp)row1[n++]='?';}
        CHECK(strstr(row1,"Never Gonna Give You Up")!=NULL,"%dx%d title row reads '%s'",a->w,a->h,row1);
        /* C3 hit rects */
        Hit *cl=find_hit(a,H_POPUP_CLOSE),*rs=find_hit(a,H_POPUP_RESIZE),*sk=find_hit(a,H_POPUP_SEEK);
        CHECK(cl&&cl->r.x==r.x+r.w-2*CELLW&&cl->r.y==r.y+CELLH&&cl->r.w==CELLW,"%dx%d close hit %d,%d %dx%d",a->w,a->h,cl?cl->r.x:-1,cl?cl->r.y:-1,cl?cl->r.w:-1,cl?cl->r.h:-1);
        CHECK(rs&&rs->r.x==r.x+r.w-CELLW&&rs->r.y==r.y+r.h-CELLH,"%dx%d resize hit %d,%d",a->w,a->h,rs?rs->r.x:-1,rs?rs->r.y:-1);
        CHECK(sk&&sk->r.y==r.y+(hc-3)*CELLH&&sk->r.x>=r.x+CELLW&&sk->r.x+sk->r.w<=r.x+r.w-CELLW,"%dx%d seek hit %d,%d w=%d",a->w,a->h,sk?sk->r.x:-1,sk?sk->r.y:-1,sk?sk->r.w:-1);
        /* C5 no run touches the right frame column */
        int chopped=0;for(int rr=r0+1;rr<r0+hc-2;rr++)if(has_text(c0+wc-1,rr))chopped++;
        CHECK(chopped==0,"%dx%d %d rows run into the frame",a->w,a->h,chopped);
        p->media.pixels=NULL;p->open=0;
        for(int i=0;i<a->nsession;i++)if(a->sessions[i].vt)lj_vt_free(a->sessions[i].vt);if(a->messages)json_object_put(a->messages);drafts_close(a);free(a);
    }
    lj_render_close();
    fprintf(stderr,"%s popup-chrome: %d checks, %d failed (frame ring text-free, title row, close/resize/seek hits, min 10 rows, no chopped runs) at 3 sizes\n",failed?"FAIL":"PASS",checks,failed);
    return failed?1:0;
}
