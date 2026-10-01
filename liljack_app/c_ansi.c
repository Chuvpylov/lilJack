#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "c_ansi.h"
#include "c_theme.h"
#include "hui_divider.h"
#include "hui_border.h"
#include "c_render.h"  /* LJ_CELL_W / LJ_LINE_H: one source of cell geometry */
#include "c_sixel.h"   /* deepseek's encoder; this file is the PRESENTER */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#define MAX_COLS 768
#define MAX_ROWS 216
#define INPUT_SIZE 8192
#define PASTE_SIZE 65536
#define ESC_TIMEOUT 40

typedef struct {uint32_t cp,marks[3],fg,bg,paint;unsigned char wide,content;} cell;

#define MAX_BORDERS 260 /* 4 rings + 63 dividers + per-tile/status/composer rules */
typedef struct {
    int col,row,cols,rows;
    uint32_t gold,blue,phase,paint;
    int separator; /* 0 dotted frame, 1 vertical stroke, 2 horizontal */
    int thickness; /* separator width in physical host pixels */
    int inner_col,inner_row,anchor_cols,anchor_rows; /* text frame within gutter bounds */
    uint64_t surface_hash;             /* occlusion mask and physical cell size */
    char *wire;size_t bytes;
} pixel_border;

/* One graphical region for the current frame. `hash` and `at` are what make a
 * static image cost nothing after its first frame. */
/* Per-frame image slots. A frame declares the popup video, one prompt mark per
 * tile, the three header graphs and the docked media tile, in that order; with
 * 8 slots and five tiles the GRAPHS lost their slots the moment a video opened
 * and their cells stayed black — the operator: "when I open video, gaps between GPU
 * CPU and MEM plots have bigger black gaps". 32 covers a full room; a frame
 * that still overflows says so once on stderr under LILJACK_TRACE_OVERLAYS. */
#define MAX_IMAGES 32
#define IMAGE_COPY_CAP (4u*1024u*1024u) /* per-slot source bytes; pool <=32 MiB */
#define SIXEL_CAP (1024*1024)
typedef struct {
    int col,row,cols,rows;              /* placement, in CELLS */
    uint32_t *src;size_t capacity;int sw,sh; /* owned reusable source snapshot */
    unsigned long long hash;            /* sampled digest of src */
    int used;
    int dmg,dx0,dy0,dx1,dy1;            /* lj_ansi_image_damage, in cells relative to col/row */
} image;

static struct {
    int opened,flags,cols,rows,physical_cols,physical_rows,failed;
    struct termios saved;
    cell *canvas,*previous;
    unsigned char input[INPUT_SIZE+1];size_t input_len;
    char paste[PASTE_SIZE+1];size_t paste_len;int pasting,paste_match,paste_bad;
    unsigned mouse_buttons;SDL_Keymod modifiers;int effects;
    long long esc_since;
    int discarding_string;
    int message_input,raw_pasting;long long raw_since;
    SDL_Event pending;int has_pending;
    int sixel;                          /* -1 unprobed, 0 no, 1 yes */
    int cellw,cellh;                    /* the HOST terminal's cell, in pixels */
    int vs16_wide;                      /* 1: the host advances two cells for U+1F6E0+U+FE0F (owkTerm); 0: one (WezTerm) */
    image images[MAX_IMAGES];int image_count,image_oversize_warned;
    unsigned long long shown[MAX_IMAGES];long long shown_at[MAX_IMAGES];
    int shown_col[MAX_IMAGES],shown_row[MAX_IMAGES];
    int shown_cols[MAX_IMAGES],shown_rows[MAX_IMAGES];
    /* Content changes held back while the terminal has not caught up: cell rect
     * per slot, merged across frames and sent as one patch after the ACK. */
    int pend[MAX_IMAGES],pend_x0[MAX_IMAGES],pend_y0[MAX_IMAGES],pend_x1[MAX_IMAGES],pend_y1[MAX_IMAGES];
    /* Frame ACK: after image content we ask DA1 (ESC[c); the terminal answers
     * only after it has processed everything before it. Until then new image
     * CONTENT is held, so a slow terminal never accumulates a backlog.
     * TIOCOUTQ cannot do this: it reads 0 on a pty slave (measured). */
    int ack_ok,ack_wait,ack_broken;long long ack_at;   /* ack_ok: this terminal answered DA1 at open */
    int min_ms;
    uint32_t paint;
    pixel_border borders[MAX_BORDERS],old_borders[MAX_BORDERS];
    int border_count,old_border_count;
    int pixel_mouse;               /* ?1016 accepted: SGR mouse reports carry pixels */
    int trace_overlays,trace_stderr,trace_retired;unsigned long trace_frame;
    int fencing,fence_open;                 /* synchronized output: fence the frame's first..last write */
    char trace_lead[96];
    int trace_x,trace_y,trace_w,trace_h;
} state;
static long long milliseconds(void);
void lj_ansi_trace_lead(const char *id,int x,int y,int w,int h){
    if(!state.trace_overlays||!id)return;
    state.trace_x=x;state.trace_y=y;state.trace_w=w;state.trace_h=h;
    size_t n=0;while(id[n]&&n<sizeof(state.trace_lead)-1){
        unsigned char c=(unsigned char)id[n];
        state.trace_lead[n]=(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'?c:'_';n++;
    }state.trace_lead[n]=0;
}
static void trace_overlays(const char *kind,int emitted,size_t bytes){
    if(!state.trace_overlays)return;
    int frames=0,separators=0,prepared=0;
    for(int i=0;i<state.border_count;i++){
        if(state.borders[i].separator)separators++;else frames++;
        prepared+=state.borders[i].wire!=NULL;
    }
    char line[1024];int len=snprintf(line,sizeof line,"liljack-overlays monotonic_ms=%lld frame=%lu kind=%s presenter=ansi sixel=%d cell=%dx%d border_rows=%d queued=%d frames=%d separators=%d prepared=%d emitted=%d bytes=%zu retired=%d images=%d lead=%s tile=%s rect=%d,%d,%d,%d\n",
        milliseconds(),
        state.trace_frame,kind,state.sixel,state.cellw,state.cellh,
        getenv("LILJACK_ANSI_BORDER_ROWS")&&!strcmp(getenv("LILJACK_ANSI_BORDER_ROWS"),"1"),
        state.border_count,frames,separators,prepared,emitted,bytes,state.trace_retired,state.image_count,
        state.trace_lead[0]?state.trace_lead:"none",state.trace_lead[0]?state.trace_lead:"none",
        state.trace_x,state.trace_y,state.trace_w,state.trace_h);
    if(len<0)return;
    if(state.trace_stderr){fwrite(line,1,(size_t)len,stderr);fflush(stderr);}
    const char *base=getenv("LILJACK_CACHE");char fallback[PATH_MAX];
    if(!base||!*base){const char *home=getenv("HOME");if(!home||!*home)home="/tmp";snprintf(fallback,sizeof fallback,"%s/.cache/liljack",home);base=fallback;}
    mkdir(base,0700);char path[PATH_MAX];snprintf(path,sizeof path,"%s/overlays.log",base);
    int fd=open(path,O_WRONLY|O_CREAT|O_APPEND,0600);if(fd<0)return;
    struct stat st;if(fstat(fd,&st)==0&&st.st_size>1024*1024){if(ftruncate(fd,0)==0)lseek(fd,0,SEEK_SET);}
    (void)write(fd,line,(size_t)len);close(fd);
}
static long long milliseconds(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (long long)t.tv_sec*1000+t.tv_nsec/1000000;}
static int write_raw(const char *p,size_t n){while(n){ssize_t z=write(STDOUT_FILENO,p,n);if(z<0&&errno==EINTR)continue;if(z<=0)return 0;p+=z;n-=(size_t)z;}return 1;}
/* SYNCHRONIZED OUTPUT (DECSET 2026). A frame is header text plus three graph
 * images plus borders, written in several pieces, and the pty hands them to the
 * terminal in arbitrary chunks — the operator saw a header with the old GPU plot next
 * to the new CPU one. The first write of a frame opens ESC[?2026h, the end of
 * lj_ansi_present closes with ESC[?2026l, so owkTerm (and WezTerm/kitty/foot,
 * which honour the mode) present the frame as one; an unchanged frame writes
 * nothing and opens no fence. Terminals without the mode ignore both. */
static int write_all(const char *p,size_t n){
    if(state.fencing&&!state.fence_open){state.fence_open=1;if(!write_raw("\033[?2026h",8))return 0;}
    return write_raw(p,n);
}
static int fence_close(int ok){state.fencing=0;if(state.fence_open){state.fence_open=0;if(!write_raw("\033[?2026l",8))return 0;}return ok;}
/* SGR-pixel mouse (?1016) only where we KNOW the replies are pixels: owkTerm
 * exports OWKTERM=1. Not under tmux: it inherits OWKTERM from the owkTerm it
 * runs in, but answers mouse reports itself, in cells. */
static int pixel_mouse_host(void){return getenv("OWKTERM")&&!getenv("TMUX");}
int lj_ansi_mouse(int enabled){
    if(!state.opened)return 0;
    const char *sequence=enabled?"\033[?1003h\033[?1006h":"\033[?1000l\033[?1002l\033[?1003l\033[?1006l\033[?1016l";
    if(!write_all(sequence,strlen(sequence)))return 0;
    /* Pixel-precise mouse (?1016) only where we KNOW the reply is pixels: owkTerm
     * sets OWKTERM=1. Elsewhere 1016 may be ignored and cells would be read as pixels. */
    if(enabled&&pixel_mouse_host()){if(!write_all("\033[?1016h",8))return 0;state.pixel_mouse=1;}
    else state.pixel_mouse=0;
    state.mouse_buttons=0;state.modifiers=KMOD_NONE;return 1;
}
void lj_ansi_pixel_mouse(int enabled){state.pixel_mouse=enabled?1:0;}
int lj_ansi_copy(const char *text){
    if(!state.opened||!text)return 0;
    size_t length=strnlen(text,65537);if(length>65536)return 0;
    static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t capacity=8+4*((length+2)/3);char *out=malloc(capacity);if(!out)return 0;
    size_t n=7;memcpy(out,"\033]52;c;",n);
    for(size_t i=0;i<length;i+=3){
        size_t remaining=length-i;uint32_t value=(uint32_t)(unsigned char)text[i]<<16;
        if(remaining>1)value|=(uint32_t)(unsigned char)text[i+1]<<8;
        if(remaining>2)value|=(unsigned char)text[i+2];
        out[n++]=alphabet[(value>>18)&63];out[n++]=alphabet[(value>>12)&63];
        out[n++]=remaining>1?alphabet[(value>>6)&63]:'=';out[n++]=remaining>2?alphabet[value&63]:'=';
    }
    out[n++]=7;int ok=write_all(out,n);free(out);return ok;
}
static void dimensions(int *cols,int *rows){struct winsize w={0};if(ioctl(STDOUT_FILENO,TIOCGWINSZ,&w)||!w.ws_col||!w.ws_row){*cols=80;*rows=24;}else{*cols=w.ws_col;*rows=w.ws_row;}if(*cols>MAX_COLS)*cols=MAX_COLS;if(*rows>MAX_ROWS)*rows=MAX_ROWS;}
static int floor_cell(int v,int unit);   /* defined with the cell writer below */
static void erase_wide(int x,int y);     /* likewise */
/* ── the host terminal's cell size, measured ────────────────────────────────
 * ⚠ THIS IS THE WHOLE REASON AN IMAGE LANDS WHERE THE LAYOUT PUT IT. lilJack
 * lays out on a fixed 10x20 grid (LJ_CELL_W/LJ_LINE_H); the terminal's cell is
 * whatever its font makes it. Emitting sixel at lilJack's pixel size therefore
 * lands at the wrong size in any terminal whose font is not 10x20, and the
 * image creeps out of its tile or leaves a gap. We ask the terminal instead.
 *
 * CSI 16 t -> ESC [ 6 ; height ; width t   (cell size in pixels, preferred)
 * CSI 14 t -> ESC [ 4 ; height ; width t   (text area; divide by rows/cols)
 * Neither answered -> 10x20, which at least matches the layout grid. */
static int read_reply(char *buf,int cap,int ms){
    long long deadline=milliseconds()+ms;int n=0;
    while(n<cap-1){
        long long left=deadline-milliseconds();if(left<=0)break;
        struct timeval tv={.tv_sec=left/1000,.tv_usec=(left%1000)*1000};
        fd_set r;FD_ZERO(&r);FD_SET(STDIN_FILENO,&r);
        if(select(STDIN_FILENO+1,&r,NULL,NULL,&tv)<=0)break;
        ssize_t z=read(STDIN_FILENO,buf+n,(size_t)(cap-1-n));
        if(z<=0)break;n+=(int)z;
        if(buf[n-1]=='t'||buf[n-1]=='c'||buf[n-1]=='R')break;
    }
    buf[n]=0;return n;
}
static int parse_t(const char *s,int len,int want,int *h,int *w){
    /* ESC [ <want> ; h ; w t */
    for(int i=0;i+2<len;i++){
        if((unsigned char)s[i]!=0x1b||s[i+1]!='[')continue;
        int p=i+2,v=0,got=0,vals[4],nv=0;
        while(p<len&&nv<4){
            if(s[p]>='0'&&s[p]<='9'){v=v*10+(s[p]-'0');got=1;p++;continue;}
            if(s[p]==';'||s[p]=='t'){if(got)vals[nv++]=v;v=0;got=0;if(s[p]=='t'){p++;break;}p++;continue;}
            break;
        }
        if(nv>=3&&vals[0]==want&&vals[1]>0&&vals[2]>0){*h=vals[1];*w=vals[2];return 1;}
    }
    return 0;
}
/* ⚠ THE HOST'S ADVANCE FOR AN AMBIGUOUS-WIDTH ICON IS MEASURED, NOT ASSUMED
 * (tile-header-doubled-glyphs-wezterm, 2026-09-13). lilJack reserves two cells
 * for 🛠 U+1F6E0 (icon-width contract) and used to append U+FE0F so the host
 * would render the two-cell emoji — owkTerm does (advance 2), WezTerm does not
 * (advance 1, its default unicode version ignores VS16 for width), so every run
 * continuing past the icon landed one column left there and a partial redraw
 * doubled the header ("RROLE ▾ ××"). Write the icon + VS16, ask CSI 6n where the
 * cursor is, remember the answer; the emitter then either keeps VS16 (host
 * widens) or writes the icon plus a SPACE in the tail cell (host does not), so
 * the advance is two everywhere. LJ_ANSI_VS16=0/1 overrides the probe. */
static void probe_vs16(void){
    state.vs16_wide=1;
    const char *env=getenv("LJ_ANSI_VS16");if(env&&*env){state.vs16_wide=atoi(env)!=0;return;}
    char buf[64];
    if(!write_all("\r\xf0\x9f\x9b\xa0\xef\xb8\x8f\033[6n",12))return;
    if(read_reply(buf,sizeof buf,120)>0){
        const char *p=strrchr(buf,'[');int row=0,col=0;
        if(p&&sscanf(p+1,"%d;%dR",&row,&col)==2&&col>=1)state.vs16_wide=(col-1)>=2;
    }
    write_all("\r\033[2K",5);
}
static void probe_pixels(void){
    state.cellw=LJ_CELL_W;state.cellh=LJ_LINE_H;
    char buf[128];int hh,ww;
    if(write_all("\033[16t",5)&&read_reply(buf,sizeof buf,120)&&parse_t(buf,(int)strlen(buf),6,&hh,&ww)){
        if(ww>0&&ww<=64&&hh>0&&hh<=128){state.cellw=ww;state.cellh=hh;return;}
    }
    if(write_all("\033[14t",5)&&read_reply(buf,sizeof buf,120)&&parse_t(buf,(int)strlen(buf),4,&hh,&ww)){
        int c=state.physical_cols>0?state.physical_cols:80,r=state.physical_rows>0?state.physical_rows:24;
        int cw=ww/c,ch=hh/r;
        if(cw>0&&cw<=64&&ch>0&&ch<=128){state.cellw=cw;state.cellh=ch;}
    }
}
int lj_ansi_images_available(void){return state.sixel==1;}
int lj_ansi_vs16_wide(void){return state.vs16_wide;}
void lj_ansi_cell_pixels(int *w,int *h){if(w)*w=state.cellw?state.cellw:LJ_CELL_W;if(h)*h=state.cellh?state.cellh:LJ_LINE_H;}

/* Sampled digest: a video frame must be seen to change, but hashing a whole
 * 1280x720 buffer every frame is itself the CPU cost the operator told us to avoid.
 * At most ~4096 samples, spread by a stride that is coprime-ish with the row. */
static unsigned long long sample_hash(const uint32_t *p,int w,int h){
    unsigned long long v=1469598103934665603ULL;size_t n=(size_t)w*h;
    size_t step=n>4096?n/4096:1;
    for(size_t i=0;i<n;i+=step){v^=p[i];v*=1099511628211ULL;}
    v^=(unsigned long long)w*31+(unsigned long long)h;return v;
}

/* ⚠ HALF BLOCKS ARE THE FALLBACK THAT ALWAYS WORKS, AND WITHOUT ONE THE VIDEO
 * SIMPLY DOES NOT EXIST FOR MOST USERS. Sixel is the good path and it is
 * DETECTED, never assumed — but a terminal that answers DA1 without attribute 4
 * then got nothing at all, and "nothing at all" is what the operator kept reporting
 * while running mate-terminal. U+2580 (▀) paints the TOP half of a cell in the
 * foreground colour and the bottom half in the background, so one cell carries
 * TWO vertical pixels in full 24-bit colour. Resolution is halved vertically;
 * everything else about it is ordinary cell content, bounded by the grid and
 * diffed by the same writer as text. This is the same technique owkterm_vt
 * already uses to render sixel an AGENT draws — the renderer existed, it was
 * simply never pointed at our own media. */
static void half_block_image(int col,int row,int cols,int rows,
                             const uint32_t *src,int sw,int sh){
    for(int ry=0;ry<rows;ry++){
        int cy=row+ry;
        if(cy<0||cy>=state.rows)continue;
        for(int rx=0;rx<cols;rx++){
            int cx=col+rx;
            if(cx<0||cx>=state.cols)continue;
            /* two source rows per cell: upper -> fg, lower -> bg */
            int sy0=(int)((long long)(ry*2)*sh/(rows*2));
            int sy1=(int)((long long)(ry*2+1)*sh/(rows*2));
            int sx=(int)((long long)rx*sw/cols);
            if(sy0>=sh)sy0=sh-1; if(sy1>=sh)sy1=sh-1; if(sx>=sw)sx=sw-1;
            uint32_t up=src[(size_t)sy0*sw+sx]&0xffffff;
            uint32_t lo=src[(size_t)sy1*sw+sx]&0xffffff;
            cell *c=&state.canvas[(size_t)cy*state.cols+cx];
            erase_wide(cx,cy);
            c->cp=0x2580;c->fg=up;c->bg=lo;c->wide=0;
            memset(c->marks,0,sizeof c->marks);
        }
    }
}

void lj_ansi_cover(int x,int y,int w,int h){
    /* Sixel is emitted after cells. Flatten intersecting lower images into
     * cells before painting opaque overlay chrome, so they cannot cover it.
     * Uncovered images retain their full-resolution sixel path. */
    for(int i=0;i<state.image_count;){
        image im=state.images[i];
        if(x<(im.col+im.cols)*LJ_CELL_W&&x+w>im.col*LJ_CELL_W&&
           y<(im.row+im.rows)*LJ_LINE_H&&y+h>im.row*LJ_LINE_H){
            half_block_image(im.col,im.row,im.cols,im.rows,im.src,im.sw,im.sh);
            memmove(&state.images[i],&state.images[i+1],(size_t)(state.image_count-i-1)*sizeof(image));
            state.image_count--;
            state.images[state.image_count]=im; /* recycle removed slot, never alias ownership */
        }else i++;
    }
}
int lj_ansi_image(int x,int y,int w,int h,const uint32_t *src,int sw,int sh){
    if(!state.opened||!src||sw<=0||sh<=0)return 0;
    if(state.sixel!=1){
        /* No sixel: draw it as half blocks, right now, into the cell grid. */
        int col=floor_cell(x,LJ_CELL_W),row=floor_cell(y,LJ_LINE_H);
        int cols=w/LJ_CELL_W,rows=h/LJ_LINE_H;
        if(col<0){cols+=col;col=0;}if(row<0){rows+=row;row=0;}
        if(col>=state.cols||row>=state.rows)return 0;
        if(col+cols>state.cols)cols=state.cols-col;
        if(row+rows>state.rows)rows=state.rows-row;
        if(cols<1||rows<1||!state.canvas)return 0;
        half_block_image(col,row,cols,rows,src,sw,sh);
        return 1;
    }
    if(state.image_count>=MAX_IMAGES){if(!state.image_oversize_warned&&state.trace_stderr){state.image_oversize_warned=1;fprintf(stderr,"lj_ansi: frame declares more than %d images; later images are dropped\n",MAX_IMAGES);}return 0;}
    int col=floor_cell(x,LJ_CELL_W),row=floor_cell(y,LJ_LINE_H);
    int cols=w/LJ_CELL_W,rows=h/LJ_LINE_H;
    if(col<0){cols+=col;col=0;}if(row<0){rows+=row;row=0;}
    if(col>=state.cols||row>=state.rows)return 0;
    if(col+cols>state.cols)cols=state.cols-col;
    if(row+rows>state.rows)rows=state.rows-row;
    if(cols<1||rows<1)return 0;
    if((size_t)sw>IMAGE_COPY_CAP/sizeof(uint32_t)/(size_t)sh){
        if(!state.image_oversize_warned){fprintf(stderr,"lilJack: image source exceeds 4 MiB copy limit; image not queued (warning once per session).\n");state.image_oversize_warned=1;}
        return 0;
    }
    size_t bytes=(size_t)sw*(size_t)sh*sizeof(uint32_t);
    image *im=&state.images[state.image_count];
    if(bytes>im->capacity){
        uint32_t *next=realloc(im->src,bytes);if(!next)return 0;
        im->src=next;im->capacity=bytes;
    }
    memcpy(im->src,src,bytes);state.image_count++;
    im->col=col;im->row=row;im->cols=cols;im->rows=rows;
    im->sw=sw;im->sh=sh;im->used=0;im->dmg=0;
    im->hash=sample_hash(im->src,sw,sh);
    return 1;
}
void lj_ansi_image_damage(int sx,int sy,int sw,int sh){
    if(!state.opened||state.sixel!=1||state.image_count<1||sw<=0||sh<=0)return;
    image *im=&state.images[state.image_count-1];
    if(im->sw<=0||im->sh<=0)return;
    /* source px -> cells of this image, rounded outward, plus one cell of margin
     * for the nearest-neighbour scale and the pen's anti-aliased edge */
    int x0=(int)((long long)sx*im->cols/im->sw)-1,y0=(int)((long long)sy*im->rows/im->sh)-1;
    int x1=(int)(((long long)(sx+sw)*im->cols+im->sw-1)/im->sw)+1,y1=(int)(((long long)(sy+sh)*im->rows+im->sh-1)/im->sh)+1;
    if(x0<0)x0=0;if(y0<0)y0=0;if(x1>im->cols)x1=im->cols;if(y1>im->rows)y1=im->rows;
    if(x1<=x0||y1<=y0)return;
    if(!im->dmg){im->dx0=x0;im->dy0=y0;im->dx1=x1;im->dy1=y1;im->dmg=1;return;}
    if(x0<im->dx0)im->dx0=x0;if(y0<im->dy0)im->dy0=y0;if(x1>im->dx1)im->dx1=x1;if(y1>im->dy1)im->dy1=y1;
}

/* Nearest-neighbour scale of the source into a cell-aligned target, then hand
 * the rect to deepseek's encoder. Cells [cx0,cx1) x [cy0,cy1) of the image
 * only (the whole image is 0,0,cols,rows); a patch uses the SAME source mapping
 * as the whole image, so its pixels line up exactly with what is on screen.
 * Returns bytes written, or <=0 to skip. */
static int emit_image_rect(image *im,int slot,char *scratch,uint32_t **buf,size_t *bufn,int cx0,int cy0,int cx1,int cy1){
    int tw=im->cols*state.cellw,th=im->rows*state.cellh;
    int px0=cx0*state.cellw,py0=cy0*state.cellh,pw=(cx1-cx0)*state.cellw,ph=(cy1-cy0)*state.cellh;
    if(tw<=0||th<=0||pw<=0||ph<=0)return 0;
    size_t need=(size_t)pw*ph;
    if(need>*bufn){uint32_t *nb=realloc(*buf,need*sizeof(uint32_t));if(!nb)return 0;*buf=nb;*bufn=need;}
    uint32_t *dst=*buf;
    for(int ty=0;ty<ph;ty++){
        int sy=(int)((long long)(py0+ty)*im->sh/th);if(sy>=im->sh)sy=im->sh-1;
        const uint32_t *srow=im->src+(size_t)sy*im->sw;uint32_t *drow=dst+(size_t)ty*pw;
        for(int tx=0;tx<pw;tx++){
            int sx=(int)((long long)(px0+tx)*im->sw/tw);if(sx>=im->sw)sx=im->sw-1;
            drow[tx]=srow[sx];
        }
    }
    int n=lj_sixel_encode(dst,pw,ph,0,0,pw,ph,scratch,SIXEL_CAP);
    if(n<=0)return 0;                 /* over the cap: drop, never truncate */
    char pos[32];int pn=snprintf(pos,sizeof pos,"\033[%d;%dH",im->row+cy0+1,im->col+cx0+1);
    /* ESC 7 / ESC 8 keep the sixel from disturbing the cell writer's idea of
     * where the cursor is on the NEXT frame. */
    if(!write_all("\0337",2))return -1;
    if(!write_all(pos,(size_t)pn))return -1;
    if(!write_all(scratch,(size_t)n))return -1;
    if(!write_all("\0338",2))return -1;
    state.shown[slot]=im->hash;state.shown_at[slot]=milliseconds();
    state.shown_col[slot]=im->col;state.shown_row[slot]=im->row;
    state.shown_cols[slot]=im->cols;state.shown_rows[slot]=im->rows;
    return n;
}
/* The whole image (tests call this directly). */
static int emit_image(image *im,int slot,char *scratch,uint32_t **buf,size_t *bufn){
    return emit_image_rect(im,slot,scratch,buf,bufn,0,0,im->cols,im->rows);
}

int lj_ansi_open(int *pixelw,int *pixelh){
    if(state.opened)return 0;
    if(!isatty(STDIN_FILENO)||!isatty(STDOUT_FILENO)){fprintf(stderr,"lilJack ANSI mode needs an interactive terminal on stdin and stdout.\n");return 0;}
    memset(&state,0,sizeof state);
    if(tcgetattr(STDIN_FILENO,&state.saved)||(state.flags=fcntl(STDIN_FILENO,F_GETFL))<0){fprintf(stderr,"Cannot read terminal settings.\n");return 0;}
    struct termios raw=state.saved;
    raw.c_iflag&=~(IGNBRK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL|IXON);
    raw.c_oflag&=~OPOST;raw.c_lflag&=~(ECHO|ECHONL|ICANON|ISIG|IEXTEN);raw.c_cflag&=~(CSIZE|PARENB);raw.c_cflag|=CS8;raw.c_cc[VMIN]=0;raw.c_cc[VTIME]=0;
    if(tcsetattr(STDIN_FILENO,TCSANOW,&raw)){fprintf(stderr,"Cannot enter raw terminal mode.\n");return 0;}
    /* VMIN=VTIME=0 makes reads return immediately. Avoid O_NONBLOCK here:
     * stdin/stdout often share an open-file description, including its flags. */
    state.opened=1;dimensions(&state.physical_cols,&state.physical_rows);
    const char *trace=getenv("LILJACK_TRACE_OVERLAYS");
    state.trace_overlays=!trace||strcmp(trace,"0");
    state.trace_stderr=trace&&(!strcmp(trace,"1"))&&!isatty(STDERR_FILENO);
    if(pixelw)*pixelw=state.physical_cols*LJ_CELL_W;if(pixelh)*pixelh=state.physical_rows*LJ_LINE_H;
    /* ⚠ PROBE BEFORE THE ALT SCREEN AND BEFORE MOUSE REPORTING. Both queries
     * are answered on stdin, and once ?1003h is on, a stray mouse report can
     * arrive between the query and its reply. Raw mode is already set above,
     * which is what makes the reply readable at all. */
    state.sixel=lj_sixel_supported(STDIN_FILENO)?1:0;
    state.ack_ok=state.sixel==1;state.ack_wait=state.ack_broken=0;   /* it answered DA1, so it can ACK frames */
    {const char *ack=getenv("LJ_SIXEL_ACK");if(ack&&!strcmp(ack,"0"))state.ack_ok=0;}   /* flow control off (tests of other things) */
    probe_pixels();probe_vs16();
    {const char *ms=getenv("LJ_SIXEL_MIN_MS");int v=ms?atoi(ms):60;state.min_ms=v<0?0:v;}
    if(getenv("LJ_SIXEL_OFF"))state.sixel=0;
    /* ⚠ NEVER SET DECSDM (?80h) HERE. Measured 2026-09-12 on :0: WezTerm
     * implements DEC's original "sixel display mode" — every image is drawn at
     * the TOP-LEFT of the screen, cursor ignored — so lilJack's overlays would
     * all pile up in the corner. Terminals that scroll after an image on the
     * last row (xterm default) are handled by placing the ring inside
     * DECSC/DECRC and by owkTerm's VT clamping instead of scrolling. */
    const char *setup="\033[?1049h\033[?25l\033[?1003h\033[?1006h\033[?2004h\033[0m\033[2J\033[H";
    if(!write_all(setup,strlen(setup))){lj_ansi_close();return 0;}
    /* owkTerm implements DECSET 1016 and exports OWKTERM. Enable pixel SGR
     * coordinates at startup, not only after the F8 mouse-capture toggle. */
    if(pixel_mouse_host()){if(!write_all("\033[?1016h",8)){lj_ansi_close();return 0;}state.pixel_mouse=1;}
    return 1;
}
static void cache_border_wires(void);
void lj_ansi_close(void){
    for(int i=0;i<MAX_IMAGES;i++)free(state.images[i].src);
    if(state.opened){const char *restore="\033[0m\033[?2004l\033[?1016l\033[?1006l\033[?1003l\033[?25h\033[?1049l";   /* 1016l: next app gets cells again */write_all(restore,strlen(restore));tcsetattr(STDIN_FILENO,TCSANOW,&state.saved);fcntl(STDIN_FILENO,F_SETFL,state.flags);}
    for(int i=0;i<state.border_count;i++)free(state.borders[i].wire);
    for(int i=0;i<state.old_border_count;i++)free(state.old_borders[i].wire);
    free(state.canvas);free(state.previous);memset(&state,0,sizeof state);
}
void lj_ansi_begin(int w,int h){
    state.trace_frame++;state.trace_lead[0]=0;state.trace_retired=state.old_border_count;
    state.trace_x=state.trace_y=state.trace_w=state.trace_h=0;
    cache_border_wires();
    state.border_count=0;state.paint=0;
    int cols=w/LJ_CELL_W,rows=h/LJ_LINE_H;if(cols<1)cols=1;if(rows<1)rows=1;if(cols>MAX_COLS)cols=MAX_COLS;if(rows>MAX_ROWS)rows=MAX_ROWS;
    if(cols!=state.cols||rows!=state.rows){
        size_t count=(size_t)cols*rows;cell *next=calloc(count,sizeof(cell)),*prev=malloc(count*sizeof(cell));
        if(!next||!prev){free(next);free(prev);state.failed=1;return;}
        memset(prev,0xff,count*sizeof(cell));free(state.canvas);free(state.previous);state.canvas=next;state.previous=prev;state.cols=cols;state.rows=rows;
    }
    for(int i=0;i<state.cols*state.rows;i++)state.canvas[i]=(cell){.cp=' ',.fg=lj_theme_rgb(LJ_THEME_CANVAS_FG),.bg=lj_theme_rgb(LJ_THEME_CANVAS_BG)};state.failed=0;
    state.image_count=0;   /* regions are declared fresh every frame */
}
static int floor_cell(int v,int unit){return v>=0?v/unit:(int)(((long long)v-unit+1)/unit);}
static void erase_wide(int x,int y){
    cell *c=&state.canvas[(size_t)y*state.cols+x];
    if(c->wide==2&&x+1<state.cols){cell *r=c+1;r->cp=' ';r->wide=0;r->content=0;memset(r->marks,0,sizeof r->marks);}
    if(c->wide==1&&x>0){cell *l=c-1;l->cp=' ';l->wide=0;l->content=0;memset(l->marks,0,sizeof l->marks);}
}
void lj_ansi_glyph(int x,int y,uint32_t cp,uint32_t rgb,int cells){
    int cx=floor_cell(x,LJ_CELL_W),cy=floor_cell(y,LJ_LINE_H);if(!state.canvas||cx<0||cy<0||cx>=state.cols||cy>=state.rows)return;
    if(cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))cp=0xfffd;
    cell *c=&state.canvas[(size_t)cy*state.cols+cx];c->paint=++state.paint;
    if(cp==0xFE0F){ /* emoji presentation: widen this cell to two, do not store the selector */
        if(c->content&&c->cp&&c->wide==0&&cx+1<state.cols){erase_wide(cx+1,cy);c->wide=2;cell *r=c+1;r->cp=0;r->wide=1;r->content=1;r->paint=c->paint;r->bg=c->bg;r->fg=c->fg;memset(r->marks,0,sizeof r->marks);}
        return;}
    if(wcwidth((wchar_t)cp)==0){c->content=1;for(int i=0;i<3;i++)if(!c->marks[i]){c->marks[i]=cp;break;}return;}
    erase_wide(cx,cy);if(cells==2&&cx+1>=state.cols){cp=0xfffd;cells=1;}
    if(cells==2)erase_wide(cx+1,cy);
    c->content=1;c->cp=cp<32||cp==127?' ':cp;c->fg=rgb&0xffffff;c->wide=cells==2?2:0;memset(c->marks,0,sizeof c->marks);
    if(cells==2){cell *r=c+1;r->cp=0;r->wide=1;r->content=1;r->paint=c->paint;r->bg=c->bg;r->fg=c->fg;memset(r->marks,0,sizeof r->marks);}
}
int lj_ansi_dot(int x,int y,uint32_t rgb){
    int cx=floor_cell(x,LJ_CELL_W),cy=floor_cell(y,LJ_LINE_H);
    if(!state.canvas||cx<0||cy<0||cx>=state.cols||cy>=state.rows)return 0;
    cell *c=&state.canvas[(size_t)cy*state.cols+cx];
    /* Spaces inside labels and wide-character continuations belong to text
     * too. Rect-painted rules/backgrounds are safe decoration surfaces. */
    if(c->content||c->wide||c->marks[0])return 0;
    c->cp=lj_theme_codepoint(LJ_THEME_RING_DOT);c->fg=rgb&0xffffff;return 1;
}
void lj_ansi_separator_hot(int x,int y,int w,int h,uint32_t rgb,int vertical,int hot){
    if(!state.canvas||w<=0||h<=0)return;
    hui_divider_rect line=hui_divider_stroke((hui_divider_rect){x,y,w,h},vertical);
    int x0=floor_cell(line.x,LJ_CELL_W),y0=floor_cell(line.y,LJ_LINE_H);
    int x1=floor_cell(line.x+line.w-1,LJ_CELL_W),y1=floor_cell(line.y+line.h-1,LJ_LINE_H);
    if(x0<0)x0=0;if(y0<0)y0=0;if(x1>=state.cols)x1=state.cols-1;if(y1>=state.rows)y1=state.rows-1;
    if(x1<x0||y1<y0)return;
    int cw,ch;lj_ansi_cell_pixels(&cw,&ch);int sixel=lj_ansi_images_available();
    uint32_t cp=sixel?' ':hui_divider_glyph(vertical,cw,ch),paint=++state.paint;
    for(int yy=y0;yy<=y1;yy++)for(int xx=x0;xx<=x1;xx++){
        cell *c=&state.canvas[(size_t)yy*state.cols+xx];
        if(c->content||c->wide||c->marks[0])continue;
        c->cp=cp;c->fg=rgb&0xffffff;c->paint=paint;
    }
    if(sixel&&state.border_count<MAX_BORDERS)
        state.borders[state.border_count++]=(pixel_border){.col=x0,.row=y0,.cols=x1-x0+1,.rows=y1-y0+1,
            .gold=rgb&0xffffff,.paint=paint,.separator=vertical?1:2,.thickness=lj_theme_int(hot?LJ_THEME_DIVIDER_HOT_PX:LJ_THEME_DIVIDER_IDLE_PX)};
}
void lj_ansi_separator(int x,int y,int w,int h,uint32_t rgb,int vertical){
    lj_ansi_separator_hot(x,y,w,h,rgb,vertical,0);
}
void lj_ansi_rect(int x,int y,int w,int h,uint32_t rgb){
    uint32_t paint=++state.paint;
    if(!state.canvas||w<=0||h<=0)return;
    long long right=(long long)x+w-1,bottom=(long long)y+h-1;
    int x0=floor_cell(x,LJ_CELL_W),y0=floor_cell(y,LJ_LINE_H),x1=(int)(right<0?-1:right/LJ_CELL_W),y1=(int)(bottom<0?-1:bottom/LJ_LINE_H);
    if(x0<0)x0=0;if(y0<0)y0=0;if(x1>=state.cols)x1=state.cols-1;if(y1>=state.rows)y1=state.rows-1;
    for(int yy=y0;yy<=y1;yy++)for(int xx=x0;xx<=x1;xx++){
        cell *c=&state.canvas[(size_t)yy*state.cols+xx];erase_wide(xx,yy);c->paint=paint;
        if(h<10||w<5){c->cp=lj_theme_codepoint(h<10?LJ_THEME_RULE_H:LJ_THEME_RULE_V);c->fg=rgb&0xffffff;}
        else {c->cp=' ';c->bg=rgb&0xffffff;}
        c->wide=0;c->content=0;memset(c->marks,0,sizeof c->marks);
    }
}
static uint32_t decode(const unsigned char *s,size_t len,size_t *used){
    uint32_t c=s[0];int n=1;
    if(c<128){*used=1;return c;}
    if(c>=0xc2&&c<=0xdf){c&=31;n=2;}else if(c>=0xe0&&c<=0xef){c&=15;n=3;}else if(c>=0xf0&&c<=0xf4){c&=7;n=4;}else{*used=1;return 0xfffd;}
    if(len<(size_t)n){*used=0;return 0;}
    for(int i=1;i<n;i++){if((s[i]&0xc0)!=0x80){*used=1;return 0xfffd;}c=(c<<6)|(s[i]&63);}
    if((n==2&&c<128)||(n==3&&c<2048)||(n==4&&c<65536)||c>0x10ffff||(c>=0xd800&&c<=0xdfff)){*used=1;return 0xfffd;}
    *used=(size_t)n;return c;
}
static size_t encode(char *s,uint32_t cp){
    if(cp<128){s[0]=(char)cp;return 1;}if(cp<2048){s[0]=(char)(192|(cp>>6));s[1]=(char)(128|(cp&63));return 2;}
    if(cp<65536){s[0]=(char)(224|(cp>>12));s[1]=(char)(128|((cp>>6)&63));s[2]=(char)(128|(cp&63));return 3;}
    s[0]=(char)(240|(cp>>18));s[1]=(char)(128|((cp>>12)&63));s[2]=(char)(128|((cp>>6)&63));s[3]=(char)(128|(cp&63));return 4;
}
void lj_ansi_text(int x,int y,const char *s,uint32_t rgb,int maxw){
    if(!s)return;size_t len=strlen(s),used=0;int pos=0,previous=0;
    while(len){uint32_t cp=decode((const unsigned char*)s,len,&used);if(!used)break;s+=used;len-=used;
        /* icon-width-contract: the icon set is TWO cells everywhere (c_render.c LJ_WIDE
         * closes U+1F6E0..U+1F6EA); glibc says 1 for those, so widen here and let the
         * encoder add U+FE0F for the host. Kept local: c_ansi.c links without c_render. */
        int cells=(cp>=0x1F6E0&&cp<=0x1F6EA)?2:wcwidth((wchar_t)cp);if(cells<0)cells=1;if(cells>2)cells=2;
        if(maxw>0&&pos+cells*LJ_CELL_W>maxw)break;
        lj_ansi_glyph(x+(cells?pos:previous),y,cp,rgb,cells?cells:1);if(cells){previous=pos;pos+=cells*LJ_CELL_W;}
    }
}
/* Drawn here, at the end of the frame, so the shortfall lands on top of whatever
 * the layout already drew and the layout owner never has to call it. Reporting,
 * not clamping: see LJ_ANSI_MIN_COLS in the header. */
static void small_terminal_notice(void){
    if(state.cols>=LJ_ANSI_MIN_COLS&&state.rows>=LJ_ANSI_MIN_ROWS)return;
    char msg[160];
    snprintf(msg,sizeof msg,"terminal %dx%d \u00b7 lilJack needs %dx%d \u00b7 composer hidden",
             state.cols,state.rows,LJ_ANSI_MIN_COLS,LJ_ANSI_MIN_ROWS);
    int y=(state.rows-1)*LJ_LINE_H,width=state.cols*LJ_CELL_W;
    lj_ansi_rect(0,y,width,20,lj_theme_rgb(LJ_THEME_NOTICE_BG));lj_ansi_text(0,y,msg,lj_theme_rgb(LJ_THEME_NOTICE_FG),width);
}
static uint32_t dim_channels(uint32_t c){
    unsigned r=(c>>16)&255,g=(c>>8)&255,b=c&255;
    return ((r*3/4)<<16)|((g*3/4)<<8)|(b*3/4);
}
void lj_ansi_effects(int enabled){state.effects=enabled?1:0;}
void lj_ansi_cursor(int x,int y,int cells){
    int cx=floor_cell(x,LJ_CELL_W),cy=floor_cell(y,LJ_LINE_H);
    if(!state.canvas||cx<0||cy<0||cx>=state.cols||cy>=state.rows)return;
    for(int i=0;i<(cells==2?2:1)&&cx+i<state.cols;i++){
        cell *c=&state.canvas[(size_t)cy*state.cols+cx+i];
        c->content=1;c->paint=++state.paint; /* visible cursor owns its cell */
        uint32_t fg=c->fg;c->fg=c->bg;c->bg=fg;   /* glyph and marks untouched */
    }
}
/* Scanline over alternate rows. Only the background is touched: cp and marks are
 * left alone, so this cannot eat a glyph however tight the layout gets. */
static void effects_pass(void){
    if(!state.effects||!state.canvas)return;
    for(int y=1;y<state.rows;y+=2)for(int x=0;x<state.cols;x++)
        state.canvas[(size_t)y*state.cols+x].bg=dim_channels(state.canvas[(size_t)y*state.cols+x].bg);
}

int lj_ansi_border(int x,int y,int w,int h,uint32_t gold,uint32_t blue,uint32_t phase){
    if(!state.opened||!lj_ansi_images_available()||state.border_count==MAX_BORDERS)return 0;
    hui_border_grid frame;
    if(!hui_border_cells(x,y,w,h,LJ_CELL_W,LJ_LINE_H,&frame))return 0;
    if(frame.col<0||frame.row<0||frame.col+frame.cols>state.cols||frame.row+frame.rows>state.rows)return 0;
    int col=frame.col>0?frame.col-1:0,row=frame.row>0?frame.row-1:0;
    int endcol=frame.col+frame.cols,endrow=frame.row+frame.rows;
    if(endcol<state.cols)endcol++;if(endrow<state.rows)endrow++;
    state.borders[state.border_count++]=(pixel_border){.col=col,.row=row,.cols=endcol-col,.rows=endrow-row,
        .inner_col=frame.col-col,.inner_row=frame.row-row,.anchor_cols=frame.cols,.anchor_rows=frame.rows,
        .gold=gold,.blue=blue,.phase=phase,.paint=state.paint};
    return 1;
}

/* c_sixel is intentionally unchanged: it emits opaque RGB, not alpha. Encode
 * a sparse edge strip with a sentinel colour, then remove only that colour's
 * data passes and select transparent-background DCS mode. Palette definitions
 * remain intact. No pixels in the interior of the tile are written. */
#define BORDER_CLEAR 0x010203u
static int sparse_strip(const uint32_t *pixels,int w,int h,char *raw,char *out,size_t cap){
    int n=lj_sixel_encode(pixels,w,h,0,0,w,h,raw,SIXEL_CAP);
    if(n<8||memcmp(raw,"\033P0;0;0q",8))return 0;
    int clear=-1,matches=0;
    for(int i=8;i<n;i++)if(raw[i]=='#'){
        int index,r,g,b,used=0;
        if(sscanf(raw+i,"#%d;2;%d;%d;%d%n",&index,&r,&g,&b,&used)==4&&used){
            if(r==0&&g==0&&b==1){clear=index;matches++;}i+=used-1;
        }
    }
    if(matches!=1)return 0; /* quantisation/collision: leave the ANSI fallback */
    size_t at=0;if(cap<9)return 0;memcpy(out,"\033P0;1;0q",8);at=8;
    for(int i=8;i<n;){
        if(raw[i]=='#'){
            char *end=NULL;long index=strtol(raw+i+1,&end,10);
            if(end==raw+i+1)return 0;
            if(*end!=';'&&index==clear){
                while(i<n&&raw[i]!='$')i++;
                if(i==n)return 0; /* every colour pass must reset its column */
                i++;continue;
            }
        }
        if(at>=cap)return 0;out[at++]=raw[i++];
    }
    return (int)at;
}
/* Gutter pixels may share a terminal cell with text, but lie outside the
 * framed tile's text rectangle. Only later overlays/images occlude the base
 * line. Dots and cell separators retain the stricter content guard below. */
static int border_surface_covered(const pixel_border *b,int col,int row){
    cell *c=&state.canvas[(size_t)row*state.cols+col];
    if(c->paint>b->paint||c->cp==0x2580)return 1;
    for(int i=0;i<state.image_count;i++){
        image *im=&state.images[i];
        if(col>=im->col&&col<im->col+im->cols&&row>=im->row&&row<im->row+im->rows)return 1;
    }
    return 0;
}
static int border_covered(const pixel_border *b,int col,int row){
    cell *c=&state.canvas[(size_t)row*state.cols+col];
    return c->content||c->wide||c->marks[0]||border_surface_covered(b,col,row);
}
/* Some receivers replace a cell's image when another sparse image covers it.
 * Carry intersecting divider pixels in each ring strip as well, so either
 * image contains the complete gutter. Dot pixels themselves always win. */
static void compose_separators(uint32_t *pixels,int w,int h,int gx,int gy,int cw,int ch){
    for(int i=0;i<state.border_count;i++){
        pixel_border *s=&state.borders[i];if(!s->separator)continue;
        int vertical=s->separator==1,sw=s->cols*cw,sh=s->rows*ch;
        int thick=s->thickness?s->thickness:1,extent=vertical?sw:sh;if(thick>extent)thick=extent;
        int left=s->col*cw+(vertical?(sw-thick+1)/2:0);
        int top=s->row*ch+(vertical?0:(sh-thick+1)/2);
        int right=left+(vertical?thick:sw),bottom=top+(vertical?sh:thick);
        if(left<gx)left=gx;if(top<gy)top=gy;if(right>gx+w)right=gx+w;if(bottom>gy+h)bottom=gy+h;
        for(int y=top;y<bottom;y++)for(int x=left;x<right;x++){
            uint32_t *pixel=&pixels[(size_t)(y-gy)*w+x-gx];
            if(*pixel==BORDER_CLEAR&&!border_covered(s,x/cw,y/ch))*pixel=s->gold;
        }
    }
}
/* ⚠ A STRIP'S FOOTPRINT MUST NEVER CROSS A QUEUED IMAGE (popup-video-torn-wezterm,
 * claude 2026-09-13). A strip's pixels under an image are already transparent,
 * but WezTerm replaces a cell's image attachment under ANY later sixel
 * footprint, so the divider and ring strips running through the video popup
 * knocked the popup image out of every cell they crossed and the previous
 * frame's pieces showed through (owkTerm keeps the earlier image under a
 * transparent later one, which is why it never showed there). Emit each strip
 * as segments along its long axis, skipping the cell runs a queued image covers:
 * same pixels, same transparency, no DCS whose footprint intersects an image. */
static int strip_cell_covered(int col,int row){
    for(int i=0;i<state.image_count;i++){const image *im=&state.images[i];
        if(col>=im->col&&col<im->col+im->cols&&row>=im->row&&row<im->row+im->rows)return 1;}
    /* ⚠ A WIDE COLOUR EMOJI IS AN IMAGE TO WEZTERM: a later sixel footprint over its
     * cell drops the glyph while plain text survives (own-window experiment,
     * docs/reports/2026-09-13-tile-header-icons/). The lead ring hid the claude
     * tile's 🧠 and the status rules hid deepseek's 🔎. Skip those cells too; the
     * strip's pixels there are transparent anyway. */
    if(col>=0&&row>=0&&col<state.cols&&row<state.rows&&state.canvas[(size_t)row*state.cols+col].wide)return 1;
    return 0;
}
static int strip_run_covered(int col,int row,int cols,int rows){
    for(int y=row;y<row+rows;y++)for(int x=col;x<col+cols;x++)if(strip_cell_covered(x,y))return 1;
    return 0;
}
static int emit_strip_segments(char *wire,size_t *total,const uint32_t *pixels,int sw,int sh,int col,int row,int cw,int ch,char *raw){
    int cols=sw/cw,rows=sh/ch;if(cols<1)cols=1;if(rows<1)rows=1;
    int vertical=rows>=cols,len=vertical?rows:cols;
    /* ⚠ A SIXEL IS A WHOLE NUMBER OF 6 px BANDS: a 20 px strip encodes as 24 px and the host
     * lays the last 4 px into the NEXT cell row. WezTerm then treats that row's cells as under
     * an image and drops any wide colour emoji there (the ring's top strip hides the claude
     * tile's 🧠, the status rule hides deepseek's 🔎; docs/reports/2026-09-13-tile-header-icons/).
     * A spill-aware split was tried and rejected: it skips the ring's top dots above every icon
     * on EVERY host, and owkTerm is the target. WezTerm-only limitation, recorded in the report. */
    uint32_t *seg=NULL;
    for(int a=0;a<len;){
        if(strip_run_covered(col+(vertical?0:a),row+(vertical?a:0),vertical?cols:1,(vertical?1:rows))){a++;continue;}
        int b=a+1;while(b<len&&!strip_run_covered(col+(vertical?0:b),row+(vertical?b:0),vertical?cols:1,(vertical?1:rows)))b++;
        int segw=vertical?sw:(b-a)*cw,segh=vertical?(b-a)*ch:sh;const uint32_t *src=pixels;
        if(vertical)src=pixels+(size_t)a*ch*sw;
        else{uint32_t *next=realloc(seg,(size_t)segw*segh*sizeof *seg);if(!next){free(seg);return 0;}seg=next;
            for(int y=0;y<segh;y++)memcpy(seg+(size_t)y*segw,pixels+(size_t)y*sw+(size_t)a*cw,(size_t)segw*sizeof *seg);src=seg;}
        char pos[64];int pn=snprintf(pos,sizeof pos,"\0337\033[%d;%dH",row+(vertical?a:0)+1,col+(vertical?0:a)+1);
        if(*total+(size_t)pn+2>=SIXEL_CAP){free(seg);return 0;}
        memcpy(wire+*total,pos,(size_t)pn);
        int n=sparse_strip(src,segw,segh,raw,wire+*total+pn,SIXEL_CAP-*total-(size_t)pn-2);
        if(n<=0){free(seg);return 0;}
        *total+=(size_t)pn+(size_t)n;memcpy(wire+*total,"\0338",2);*total+=2;
        a=b;
    }
    free(seg);return 1;
}
static int prepare_separator(pixel_border *b){
    int cw,ch;lj_ansi_cell_pixels(&cw,&ch);int w=b->cols*cw,h=b->rows*ch;
    if(w<1||h<1||(size_t)w*h>1048576u)return 0;
    uint32_t *pixels=malloc((size_t)w*h*sizeof *pixels);char *raw=malloc(SIXEL_CAP),*wire=malloc(SIXEL_CAP);
    if(!pixels||!raw||!wire){free(pixels);free(raw);free(wire);return 0;}
    int vertical=b->separator==1,thick=b->thickness?b->thickness:1;
    int extent=vertical?w:h;if(thick>extent)thick=extent;
    /* Centre in the physical cell: 17px gives 8px idle / 7px hot padding.
     * Small host cells retain centring when 5px cannot fit. */
    hui_divider_rect stroke=vertical?(hui_divider_rect){(w-thick+1)/2,0,thick,h}
                                     :(hui_divider_rect){0,(h-thick+1)/2,w,thick};
    for(int y=0;y<h;y++)for(int x=0;x<w;x++){
        cell *c=&state.canvas[(size_t)(b->row+y/ch)*state.cols+b->col+x/cw];
        uint32_t color=BORDER_CLEAR;
        if(!c->content&&!c->wide&&!c->marks[0]&&!border_covered(b,b->col+x/cw,b->row+y/ch)){
            if(x>=stroke.x&&x<stroke.x+stroke.w&&y>=stroke.y&&y<stroke.y+stroke.h)color=b->gold;
        }
        pixels[(size_t)y*w+x]=color;
    }
    size_t bytes=0;int ok=emit_strip_segments(wire,&bytes,pixels,w,h,b->col,b->row,cw,ch,raw);
    free(pixels);free(raw);if(!ok){free(wire);return 0;}
    char *small=bytes?realloc(wire,bytes):NULL;   /* an entirely covered strip emits nothing */
    free(b->wire);b->wire=small?small:wire;b->bytes=bytes;return 1;
}
static int prepare_border(pixel_border *b){
    if(b->separator)return prepare_separator(b);
    int cw,ch;lj_ansi_cell_pixels(&cw,&ch);
    int tw=b->cols*cw,th=b->rows*ch;
    if(cw<1||ch<1)return 0; /* One-pixel strokes use measured host cells. */
    size_t horizontal=(size_t)tw*ch,vertical=(size_t)cw*th;
    size_t count=horizontal>vertical?horizontal:vertical;
    if(count>1048576u)return 0;
    uint32_t *pixels=malloc(count*sizeof(*pixels));
    char *raw=malloc(SIXEL_CAP),*wire=malloc(SIXEL_CAP);
    if(!pixels||!raw||!wire){free(pixels);free(raw);free(wire);return 0;}
    /* Shared per-edge CELL anchors, never a fixed pixel pitch around the
     * perimeter. One pixel thick on both axes; corners belong to one edge.
     * Fixed anchors keep corners/gaps intact; colour phase remains animated. */
    int ac=b->anchor_cols,ar=b->anchor_rows;
    int left=b->inner_col?cw-1:0,top=b->inner_row?ch-1:0;
    int right=(b->inner_col+ac)*cw;if(right>=tw)right=tw-1;
    int bottom=(b->inner_row+ar)*ch;if(bottom>=th)bottom=th-1;
    size_t total=0;int ok=1;
    for(int side=0;side<4&&ok;side++){
        int ox=side==3?tw-cw:0,oy=side==1?th-ch:side>=2?ch:0;
        int sw=side<2?tw:cw,sh=side<2?ch:th-2*ch;
        if(sh<=0)continue;
        for(int y=0;y<sh;y++)for(int x=0;x<sw;x++){
            int px=ox+x,py=oy+y,col=b->col+px/cw,row=b->row+py/ch;
            /* ⚠ DOTS ONLY. A persistent 1px gutter line used to run under the
             * dots (0x657586); at one pixel it read as a SOLID hairline and hid
             * the dots entirely in the operator's WezTerm ("hard hairline visible,
             * dotted not visible", 2026-09-12 06:52). The lead tile must look
             * identical to every other tile except the dotted highlight. */
            (void)col;(void)row;
            pixels[(size_t)y*sw+x]=BORDER_CLEAR;
        }
        /* ⚠ THE PATTERN IS 2x2 DOTS WITH A 4 PX GAP, MARCHING (the operator, live,
         * 2026-09-12: "dotted pattern 2x2 and 4px gap, not whatever we have
         * now"). A 1 px anchor set derived from the cell grid was invisible on
         * his terminal and its spacing drifted with the tile size; a fixed
         * 6 px pitch is the same on every tile, and a 2x2 block survives the
         * host's scaling. The phase walks the pattern along the edge by one
         * pixel per tick, so the whole ring marches. */
        for(int edge=0;edge<4;edge++){
            int len=(edge&1)?bottom-top:right-left;
            if(len<=0)continue;
            int pitch=LJ_BORDER_DOT+LJ_BORDER_GAP, shift=(int)(b->phase%(uint32_t)pitch);
            for(int at=-pitch+shift;at<=len;at+=pitch){
                for(int dy=0;dy<LJ_BORDER_DOT;dy++)for(int dx=0;dx<LJ_BORDER_DOT;dx++){
                    int along=at+((edge&1)?dy:dx), across=(edge&1)?dx:dy;
                    if(along<0||along>len)continue;
                    int px=edge==0?left+along+0*across:edge==1?right-across:edge==2?right-along:left+across;
                    int py=edge==0?top+across:edge==1?top+along:edge==2?bottom-across:bottom-along;
                    /* ⚠ A DOT IS 2x2, NOT 2x1 (the operator, live 2026-09-12 20:10: "2x2px
                     * dots"). The ring's edge pixel sits on the LAST pixel of the
                     * gutter cell (top=ch-1, left=cw-1), so the second pixel of every
                     * dot fell outside the strip and was clipped: the decoded wire
                     * showed exactly one lit row per edge. Fold the dot back inside
                     * the strip instead of dropping half of it. */
                    if(px<ox)px+=LJ_BORDER_DOT;else if(px>=ox+sw)px-=LJ_BORDER_DOT;
                    if(py<oy)py+=LJ_BORDER_DOT;else if(py>=oy+sh)py-=LJ_BORDER_DOT;
                    if(px<ox||px>=ox+sw||py<oy||py>=oy+sh)continue;
                    if(edge==0&&py>top+LJ_BORDER_DOT-1)continue;      /* fold moved it, keep the 2px band */
                    if(edge==3&&px>left+LJ_BORDER_DOT-1)continue;
                    if(edge==1&&px<right-LJ_BORDER_DOT+1)continue;
                    if(edge==2&&py<bottom-LJ_BORDER_DOT+1)continue;
                    /* ⚠ A DOT NEVER LANDS ON A CELL THAT HOLDS A GLYPH. The
                     * surface test alone let the ring's strip cover a cell whose
                     * content is a WIDE glyph (an agent icon) or its
                     * continuation; re-emitting that cell erases the pair and
                     * the icon blinks — the operator live 2026-09-12: "icon issue when
                     * leader board is near". border_covered is the strict test:
                     * content, wide, marks AND surface. */
                    if(border_covered(b,b->col+px/cw,b->row+py/ch))continue;
                    /* ⚠ A DOT KEEPS ITS COLOUR WHILE IT MARCHES (the operator: "don't switch
                     * colours of dots, keep them persistent"). Parity from at/pitch
                     * flipped every time the phase wrapped, so gold and blue swapped
                     * places six ticks apart. Index the dot by its ordinal along the
                     * edge, which travels with it. */
                    /* ⚠ IDENTITY ACROSS THE WRAP (codex, seq 1352): (at-shift)/pitch
                     * discards phase/pitch, so when shift wraps 5→0 the dot that
                     * physically moves x5→x6 gets ordinal+1 and swaps colour every
                     * 6 ticks. A dot's identity is (position − phase) modulo the
                     * two-dot period 2*pitch: constant while it marches. */
                    int period=2*pitch, ident=((at-(int)(b->phase%(uint32_t)period))%period+period)%period;
                    uint32_t value=((ident/pitch)&1)?b->blue:b->gold;
                    pixels[(size_t)(py-oy)*sw+px-ox]=value;
                }
            }
        }
        if(!ok)break;
        compose_separators(pixels,sw,sh,b->col*cw+ox,b->row*ch+oy,cw,ch);
        if(!emit_strip_segments(wire,&total,pixels,sw,sh,b->col+ox/cw,b->row+oy/ch,cw,ch,raw)){ok=0;break;}
    }
    free(pixels);free(raw);
    if(!ok){free(wire);return 0;}
    free(b->wire);b->wire=wire;b->bytes=total;return 1;
}
static void retire_border(const pixel_border *b){
    for(int y=b->row;y<b->row+b->rows;y++)for(int x=b->col;x<b->col+b->cols;x++){
        if(x!=b->col&&x!=b->col+b->cols-1&&y!=b->row&&y!=b->row+b->rows-1)continue;
        if(x>=0&&y>=0&&x<state.cols&&y<state.rows)memset(&state.previous[(size_t)y*state.cols+x],0xff,sizeof(cell));
    }
}
/* Same ring, same place? Geometry only — the colours and the phase change every
 * tick by design and must not count as a move. */
static int border_same_place(const pixel_border *a,const pixel_border *b){
    return a->col==b->col&&a->row==b->row&&a->cols==b->cols&&a->rows==b->rows
        &&a->separator==b->separator&&a->thickness==b->thickness&&a->inner_col==b->inner_col&&a->inner_row==b->inner_row
        &&a->anchor_cols==b->anchor_cols&&a->anchor_rows==b->anchor_rows;
}
static int border_previous(const pixel_border *b){
    for(int i=0;i<state.old_border_count;i++){
        const pixel_border *old=&state.old_borders[i];
        if(border_same_place(old,b)&&old->gold==b->gold&&old->blue==b->blue)return i;
    }
    return -1;
}
/* The current frame owns its wires until begin moves them to the last-shown
 * cache. prepare_borders moves matching wires back; the two owners never alias. */
static void cache_border_wires(void){
    for(int i=0;i<state.border_count;i++){
        pixel_border *b=&state.borders[i];if(!b->wire)continue;
        int old=border_previous(b);
        if(old>=0){
            free(state.old_borders[old].wire);
            state.old_borders[old].wire=b->wire;state.old_borders[old].bytes=b->bytes;
        }else free(b->wire);
        b->wire=NULL;
    }
}
static uint64_t border_surface_hash(const pixel_border *b){
    int cw,ch;lj_ansi_cell_pixels(&cw,&ch);
    uint64_t hash=(1469598103934665603ULL^(unsigned)cw)*1099511628211ULL;
    hash=(hash^(unsigned)ch)*1099511628211ULL;
    if(!b->separator)for(int i=0;i<state.border_count;i++){
        const pixel_border *s=&state.borders[i];if(!s->separator)continue;
        if(s->col>=b->col+b->cols||s->col+s->cols<=b->col||s->row>=b->row+b->rows||s->row+s->rows<=b->row)continue;
        const unsigned shape[]={(unsigned)s->col,(unsigned)s->row,(unsigned)s->cols,(unsigned)s->rows,
                                (unsigned)s->separator,(unsigned)s->thickness,s->gold};
        for(size_t k=0;k<sizeof shape/sizeof shape[0];k++)hash=(hash^shape[k])*1099511628211ULL;
    }
    for(int y=b->row;y<b->row+b->rows;y++)for(int x=b->col;x<b->col+b->cols;x++){
        if(!b->separator&&x>b->col&&x<b->col+b->cols-1&&y>b->row&&y<b->row+b->rows-1)continue;
        /* Image coverage is hashed on its own: a cell an overlay had already painted
         * over hashes the same before and after a sixel lands on it, so the strip
         * prepared during the popup's "Loading video…" frames kept its whole footprint
         * once the video arrived (popup-video-torn-wezterm). The footprint segments
         * depend on the images, so the cache key must too. */
        hash=(hash^(unsigned)border_covered(b,x,y)^((unsigned)strip_cell_covered(x,y)<<1))*1099511628211ULL;
    }
    return hash;
}
/* Text and images can erase any perimeter cell; interior text cannot. */
static void damage_borders(int *damage,int x,int y,int w,int h){
    for(int i=0;i<state.border_count;i++){
        const pixel_border *b=&state.borders[i];
        int left=b->col,right=left+b->cols-1,top=b->row,bottom=top+b->rows-1;
        if(x>right||x+w<=left||y>bottom||y+h<=top)continue;
        if(b->separator||x<=left||x+w>right||y<=top||y+h>bottom)damage[i]=1;
    }
}
static void prepare_borders(void){
    /* A controller redraw is not an animation tick. Carry forward the phase
     * last written to the terminal, including intervening border-only ticks. */
    for(int i=0;i<state.border_count;i++){
        pixel_border *b=&state.borders[i];int old=border_previous(b);
        uint64_t surface=border_surface_hash(b);
        if(old>=0){
            pixel_border *cached=&state.old_borders[old];b->phase=cached->phase;
            if(!b->wire&&cached->wire&&cached->surface_hash==surface){
                b->wire=cached->wire;b->bytes=cached->bytes;cached->wire=NULL;
            }
        }
        if(b->wire&&b->surface_hash&&b->surface_hash!=surface){free(b->wire);b->wire=NULL;}
        b->surface_hash=surface;
    }
    /* ⚠ RETIRE ONLY WHAT ACTUALLY MOVED. retire_border dirties a border's whole
     * perimeter in `previous`, which forces the cell pass to re-emit every one
     * of those cells next frame. Doing that for EVERY old border on EVERY
     * present meant a lead tile that had not moved still repainted its whole
     * perimeter ~40 times a second — the redraw storm s-9b350e46 measured on
     * 2026-09-12. A ring that is still in the same place needs no dirtying: its
     * sixel is re-emitted by lj_ansi_border_tick on its own. */
    for(int i=0;i<state.old_border_count;i++){
        int still_there=0;
        for(int j=0;j<state.border_count&&!still_there;j++)
            still_there=border_same_place(&state.old_borders[i],&state.borders[j]);
        if(!still_there)retire_border(&state.old_borders[i]);
    }
    for(int i=0;i<state.border_count;i++){
        pixel_border *b=&state.borders[i];
        if(!b->wire&&!prepare_border(b))continue;
        /* Suppress coarse placeholder only once all four strips are ready.
         * ⚠ THE PLACEHOLDERS LIVE ON THE STRIP CELLS, NOT THE FRAME EDGE
         * (ring-gutter-reservation, 2026-09-13). main.c dotted_border paints
         * its '•' stand-ins on the tile's outer cells — the gutter the ring
         * owns — and that is exactly this border's outer perimeter. Clearing
         * the inset frame's edge instead left 22-56 '•' cells behind at every
         * size: a second ring in cells whose colours flip every four frames,
         * the operator's "doubled border" / "colours flip". */
        for(int y=b->row;y<b->row+b->rows;y++)for(int x=b->col;x<b->col+b->cols;x++){
            if(x!=b->col&&x!=b->col+b->cols-1&&y!=b->row&&y!=b->row+b->rows-1)continue;
            cell *c=&state.canvas[(size_t)y*state.cols+x];
            if(!c->content&&c->paint<=b->paint&&(c->cp==lj_theme_codepoint(LJ_THEME_RING_DOT)||(b->separator&&c->cp>=0x2581&&c->cp<=0x258f)))c->cp=' ';
        }
    }
}
static int emit_borders(const int *damage){
    int emitted=0;size_t bytes=0;
    for(int i=0;i<state.border_count;i++){
        pixel_border *b=&state.borders[i];if(!b->wire)continue;
        int old=border_previous(b);
        if(old<0||state.old_borders[old].surface_hash!=b->surface_hash||damage[i]){
            if(!write_all(b->wire,b->bytes))return 0;
            emitted++;bytes+=b->bytes;
        }
    }
    for(int i=0;i<state.old_border_count;i++)free(state.old_borders[i].wire);
    state.old_border_count=0;
    for(int i=0;i<state.border_count;i++){
        pixel_border *b=&state.borders[i];if(!b->wire)continue;
        state.old_borders[state.old_border_count]=*b;
        state.old_borders[state.old_border_count++].wire=NULL;
    }
    trace_overlays("present",emitted,bytes);return 1;
}
int lj_ansi_border_tick(uint32_t phase){
    if(!state.opened||!lj_ansi_images_available())return 1;
    int emitted=0;size_t bytes=0;
    for(int i=0;i<state.border_count;i++){
        pixel_border *b=&state.borders[i];
        if(b->separator||!b->wire||b->phase==phase)continue;   /* every tick moves the dots */
        uint32_t old=b->phase;b->phase=phase;
        if(!prepare_border(b)){b->phase=old;continue;} /* retain last valid overlay */
        if(!write_all(b->wire,b->bytes))return 0;
        int previous=border_previous(b);
        if(previous>=0)state.old_borders[previous].phase=phase;
        emitted++;bytes+=b->bytes;
    }
    if(emitted)trace_overlays("tick",emitted,bytes);return 1;
}
static int present_frame(void);
int lj_ansi_present(void){state.fencing=1;state.fence_open=0;return fence_close(present_frame());}
static int present_frame(void){
    if(!state.opened||!state.canvas||state.failed)return 0;
    prepare_borders();
    /* A sixel image is not stored in previous[]. On move/close we must
     * explicitly dirty its old cells, even if their text has not changed. */
    for(int k=0;k<MAX_IMAGES;k++){
        if(!state.shown_cols[k])continue;
        image *im=k<state.image_count?&state.images[k]:NULL;
        if(im&&im->col==state.shown_col[k]&&im->row==state.shown_row[k]&&
           im->cols==state.shown_cols[k]&&im->rows==state.shown_rows[k])continue;
        for(int y=state.shown_row[k];y<state.shown_row[k]+state.shown_rows[k]&&y<state.rows;y++)
            for(int x=state.shown_col[k];x<state.shown_col[k]+state.shown_cols[k]&&x<state.cols;x++)
                if(x>=0&&y>=0)memset(&state.previous[(size_t)y*state.cols+x],0xff,sizeof(cell));
        state.shown[k]=0;state.shown_cols[k]=state.shown_rows[k]=0;state.pend[k]=0;
    }
    small_terminal_notice();
    effects_pass();
    int repaint[MAX_IMAGES];memset(repaint,0,sizeof repaint);
    int border_damage[MAX_BORDERS]={0};
    char out[16384];size_t n=0;int nextx=-1,nexty=-1;uint32_t fg=UINT32_MAX,bg=UINT32_MAX;
    int n_written=0;
    for(int y=0;y<state.rows;y++)for(int x=0;x<state.cols;x++){
        size_t i=(size_t)y*state.cols+x;cell *c=&state.canvas[i];state.previous[i].paint=c->paint;if(!memcmp(c,&state.previous[i],sizeof *c))continue;
        state.previous[i]=*c;if(c->wide==1)continue;
        damage_borders(border_damage,x,y,c->wide==2?2:1,1);
        /* ⚠ A CELL REDRAWN UNDER AN IMAGE PUNCHES A HOLE IN IT. The cell pass
         * runs first and knows nothing about sixel, so any cell it repaints
         * inside a region forces that region to be re-emitted on top — without
         * this a static image slowly erodes as the UI around it updates. */
        for(int k=0;k<state.image_count;k++){
            image *im=&state.images[k];
            if(x>=im->col&&x<im->col+im->cols&&y>=im->row&&y<im->row+im->rows){repaint[k]=1;break;}
        }
        n_written=1;
        if(n>sizeof(out)-160){if(!write_all(out,n))return 0;n=0;}
        if(x!=nextx||y!=nexty)n+=(size_t)snprintf(out+n,sizeof(out)-n,"\033[%d;%dH",y+1,x+1);
        if(fg!=c->fg){n+=(size_t)snprintf(out+n,sizeof(out)-n,"\033[38;2;%u;%u;%um",(c->fg>>16)&255,(c->fg>>8)&255,c->fg&255);fg=c->fg;}
        if(bg!=c->bg){n+=(size_t)snprintf(out+n,sizeof(out)-n,"\033[48;2;%u;%u;%um",(c->bg>>16)&255,(c->bg>>8)&255,c->bg&255);bg=c->bg;}
        nextx=x+(c->wide==2?2:1);nexty=y;
        n+=encode(out+n,c->cp?c->cp:' ');
        /* icon-width-contract: a wide cell whose codepoint the host measures as 1
         * (🛠 U+1F6E0 and friends) gets U+FE0F so every terminal renders the
         * two-cell emoji presentation, matching lilJack's grid. */
        if(c->wide==2&&c->cp&&wcwidth((wchar_t)c->cp)==1){if(state.vs16_wide)n+=encode(out+n,0xFE0F);else out[n++]=' ';}   /* measured at open, see probe_vs16 */
        for(int m=0;m<3;m++)if(c->marks[m])n+=encode(out+n,c->marks[m]);
    }
    if(n&&!write_all(out,n))return 0;
    /* ⚠ PARK THE REAL CURSOR AND RE-ASSERT THAT IT IS HIDDEN. lilJack draws its
     * own cursor as a reverse-video CELL (lj_ansi_cursor), so the host's cursor
     * is hidden once at open with ?25l and never mentioned again. Two things
     * then go wrong on a host that shows one anyway:
     *   - the diff loop leaves the real cursor wherever the LAST CHANGED CELL
     *     happened to be, which moves every frame and is usually a row below
     *     the content the eye is on;
     *   - a single ?25l at open survives only until something re-enables it,
     *     and we never say it again.
     * the operator sees the agent-tile cursor one row lower in wezterm than in
     * mate-terminal, which is exactly the shape of a second, host-drawn cursor
     * at the last write position. Re-hiding costs 6 bytes and parking costs 6
     * more, only on frames that changed anything, and it makes the output
     * say the same thing to every terminal instead of depending on which one
     * honours a setting from minutes ago. */
    if(n_written){
        const char *park="\033[H\033[?25l";
        if(!write_all(park,strlen(park)))return 0;
    }
    if(state.sixel!=1){trace_overlays("present",0,0);return 1;}
    /* ── graphical regions, painted OVER the cells ─────────────────────────
     * Scratch is allocated once per present and freed here: the encoder is
     * explicitly allocation-free in its sample path, so the buffer is ours. */
    static char *scratch=NULL;static uint32_t *scaled=NULL;static size_t scaledn=0;
    if(!scratch&&!(scratch=malloc(SIXEL_CAP)))return emit_borders(border_damage);
    long long now=milliseconds();
    if(state.ack_wait&&now-state.ack_at>1000){state.ack_wait=0;state.ack_broken=1;}   /* terminal never answers: stop asking */
    int content=0;
    for(int k=0;k<state.image_count;k++){
        image *im=&state.images[k];
        int moved=state.shown_col[k]!=im->col||state.shown_row[k]!=im->row
                ||state.shown_cols[k]!=im->cols||state.shown_rows[k]!=im->rows;
        if(im->dmg&&!moved){                               /* remember it even if we hold it this frame */
            if(!state.pend[k]){state.pend_x0[k]=im->dx0;state.pend_y0[k]=im->dy0;state.pend_x1[k]=im->dx1;state.pend_y1[k]=im->dy1;state.pend[k]=1;}
            else{if(im->dx0<state.pend_x0[k])state.pend_x0[k]=im->dx0;if(im->dy0<state.pend_y0[k])state.pend_y0[k]=im->dy0;
                 if(im->dx1>state.pend_x1[k])state.pend_x1[k]=im->dx1;if(im->dy1>state.pend_y1[k])state.pend_y1[k]=im->dy1;}
        }
        int changed=state.shown[k]!=im->hash||state.pend[k];
        if(!moved&&!changed&&!repaint[k])continue;        /* damage tracking */
        int ex0=0,ey0=0,ex1=im->cols,ey1=im->rows;
        if(!moved&&!repaint[k]){
            /* CONTENT churn (video, live drawing): rate limited, and held while
             * the terminal has not acknowledged the previous content frame.
             * A move or a punched hole is a correctness repair and never waits. */
            if(state.ack_wait)continue;
            if(state.min_ms&&now-state.shown_at[k]<state.min_ms)continue;
            if(state.pend[k]&&state.shown[k]){ex0=state.pend_x0[k];ey0=state.pend_y0[k];ex1=state.pend_x1[k];ey1=state.pend_y1[k];}
        }
        int whole=ex0==0&&ey0==0&&ex1==im->cols&&ey1==im->rows;
        int emitted=whole?emit_image(im,k,scratch,&scaled,&scaledn):emit_image_rect(im,k,scratch,&scaled,&scaledn,ex0,ey0,ex1,ey1);
        if(emitted<0)return 0;
        state.pend[k]=0;
        if(emitted>0){content=1;damage_borders(border_damage,im->col+ex0,im->row+ey0,ex1-ex0,ey1-ey0);}
    }
    if(content&&state.ack_ok&&!state.ack_broken&&!state.ack_wait){
        if(!write_all("\033[c",3))return 0;
        state.ack_wait=1;state.ack_at=now;
    }
    return emit_borders(border_damage);
}
static void consume(size_t n){state.input_len-=n;memmove(state.input,state.input+n,state.input_len);state.input[state.input_len]=0;state.esc_since=0;}
static int key_event(SDL_Event *e,SDL_Keycode key,SDL_Keymod mod){memset(e,0,sizeof *e);e->type=SDL_KEYDOWN;e->key.state=SDL_PRESSED;e->key.keysym.sym=key;e->key.keysym.mod=mod;state.modifiers=mod;return 1;}
static int custom(SDL_Event *e,int code,const char *s){memset(e,0,sizeof *e);char *copy=strdup(s);if(!copy)return 0;e->type=SDL_USEREVENT;e->user.code=code;e->user.data1=copy;return 1;}
static void paste_byte(unsigned char c){if(!c||state.paste_len==PASTE_SIZE){state.paste_bad=1;return;}state.paste[state.paste_len++]=(char)c;}
static int paste_input(SDL_Event *e){
    static const char end[]="\033[201~";size_t n=0;
    while(n<state.input_len){unsigned char c=state.input[n++];
        if(c==(unsigned char)end[state.paste_match]){if(++state.paste_match==6){consume(n);state.pasting=0;state.paste[state.paste_len]=0;return custom(e,state.paste_bad?LJ_ANSI_ERROR_CODE:LJ_ANSI_PASTE_CODE,state.paste_bad?"Paste rejected: exceeds 64 KiB or contains NUL":state.paste);}}
        else {for(int i=0;i<state.paste_match;i++)paste_byte((unsigned char)end[i]);state.paste_match=0;if(c==27)state.paste_match=1;else paste_byte(c);}
    }consume(n);return 0;
}
static SDL_Keymod mods(int value){int bits=value>0?value-1:0;return (SDL_Keymod)((bits&1?KMOD_SHIFT:0)|(bits&2?KMOD_ALT:0)|(bits&4?KMOD_CTRL:0));}
static int number(const unsigned char *s,size_t len,size_t *pos,int *value){int v=0;size_t start=*pos;while(*pos<len&&s[*pos]>='0'&&s[*pos]<='9'){if(v>100000)return 0;v=v*10+s[(*pos)++]-'0';}*value=v;return *pos>start;}
static int mouse_event(SDL_Event *e,size_t end){
    size_t p=3;int b,x,y;if(!number(state.input,end,&p,&b)||p>=end||state.input[p++]!=';'||!number(state.input,end,&p,&x)||p>=end||state.input[p++]!=';'||!number(state.input,end,&p,&y)||p!=end-1||x<1||y<1||x>(state.pixel_mouse?MAX_COLS*64:MAX_COLS)||y>(state.pixel_mouse?MAX_ROWS*128:MAX_ROWS)){consume(end);return 0;}
    int release=state.input[end-1]=='m';consume(end);
    if(state.pixel_mouse){int cw=state.cellw>0?state.cellw:LJ_CELL_W,ch=state.cellh>0?state.cellh:LJ_LINE_H;x=(int)(((long)(x-1)*LJ_CELL_W+cw/2)/cw);y=(int)(((long)(y-1)*LJ_LINE_H+ch/2)/ch);}
    else {x=(x-1)*LJ_CELL_W+LJ_CELL_W/2;y=(y-1)*LJ_LINE_H+LJ_LINE_H/2;}
    state.modifiers=(SDL_Keymod)((b&4?KMOD_SHIFT:0)|(b&8?KMOD_ALT:0)|(b&16?KMOD_CTRL:0));memset(e,0,sizeof *e);
    if(b&64){e->type=SDL_MOUSEMOTION;e->motion.x=x;e->motion.y=y;e->motion.state=state.mouse_buttons;memset(&state.pending,0,sizeof state.pending);state.pending.type=SDL_MOUSEWHEEL;state.pending.wheel.y=(b&1)?-1:1;state.has_pending=1;return 1;}
    if(b&32){e->type=SDL_MOUSEMOTION;e->motion.x=x;e->motion.y=y;e->motion.state=state.mouse_buttons;return 1;}
    int button=(b&3)==0?SDL_BUTTON_LEFT:(b&3)==1?SDL_BUTTON_MIDDLE:SDL_BUTTON_RIGHT;
    if(release)state.mouse_buttons&=~SDL_BUTTON(button);else state.mouse_buttons|=SDL_BUTTON(button);
    e->type=release?SDL_MOUSEBUTTONUP:SDL_MOUSEBUTTONDOWN;e->button.button=(Uint8)button;e->button.state=release?SDL_RELEASED:SDL_PRESSED;e->button.x=x;e->button.y=y;return 1;
}
static int escape(SDL_Event *e){
    size_t len=state.input_len;
    if(len==1){if(!state.esc_since)state.esc_since=milliseconds();if(milliseconds()-state.esc_since<ESC_TIMEOUT)return 0;consume(1);return key_event(e,SDLK_ESCAPE,KMOD_NONE);}
    if(state.input[1]==']'||state.input[1]=='P'||state.input[1]=='^'||state.input[1]=='_'){consume(2);state.discarding_string=1;return 0;}
    if(state.input[1]!='['&&state.input[1]!='O'){consume(1);state.modifiers=KMOD_ALT;return -1;}
    size_t end=2;while(end<len&&!(state.input[end]>=0x40&&state.input[end]<=0x7e))end++;
    if(end==len){if(!state.esc_since)state.esc_since=milliseconds();if(len>80||milliseconds()-state.esc_since>=ESC_TIMEOUT)consume(len);return 0;}end++;
    if(state.input[1]=='['&&state.input[2]=='<')return mouse_event(e,end);
    if(state.input[1]=='['&&state.input[2]=='?'&&state.input[end-1]=='c'){state.ack_wait=0;consume(end);return 0;}   /* DA1 reply = frame ACK */
    unsigned char final=state.input[end-1];int first=1,modifier=1;size_t pos=2;
    if(pos<end-1&&state.input[pos]>='0'&&state.input[pos]<='9'){
        if(!number(state.input,end-1,&pos,&first)){consume(end);return 0;}
        if(pos<end-1&&state.input[pos]==';'){pos++;if(!number(state.input,end-1,&pos,&modifier)){consume(end);return 0;}}
    }
    if(pos!=end-1){consume(end);return 0;}
    SDL_Keycode key=0;
    if(final=='A')key=SDLK_UP;else if(final=='B')key=SDLK_DOWN;else if(final=='C')key=SDLK_RIGHT;else if(final=='D')key=SDLK_LEFT;
    else if(final=='H')key=SDLK_HOME;else if(final=='F')key=SDLK_END;else if(final=='Z'){key=SDLK_TAB;modifier=2;}
    else if(final>='P'&&final<='S')key=SDLK_F1+final-'P';
    else if(final=='~'){
        switch(first){case 1:case 7:key=SDLK_HOME;break;case 2:key=SDLK_INSERT;break;case 3:key=SDLK_DELETE;break;case 4:case 8:key=SDLK_END;break;case 5:key=SDLK_PAGEUP;break;case 6:key=SDLK_PAGEDOWN;break;
            case 11:key=SDLK_F1;break;case 12:key=SDLK_F2;break;case 13:key=SDLK_F3;break;case 14:key=SDLK_F4;break;case 15:key=SDLK_F5;break;case 17:key=SDLK_F6;break;case 18:key=SDLK_F7;break;case 19:key=SDLK_F8;break;case 20:key=SDLK_F9;break;case 21:key=SDLK_F10;break;case 23:key=SDLK_F11;break;case 24:key=SDLK_F12;break;
            case 200:state.pasting=1;state.paste_len=0;state.paste_match=state.paste_bad=0;break;default:break;}
    }
    consume(end);return key?key_event(e,key,mods(modifier)):0;
}
void lj_ansi_message_input(int enabled){state.message_input=enabled!=0;}
static int raw_paste_input(SDL_Event *e){
    if(state.input_len){
        size_t n=0;
        /* Mouse tracking can interleave with an unframed paste. Keep complete
         * SGR mouse reports in the protocol reader, outside the text payload. */
        while(n<state.input_len){
            if(n+2<state.input_len&&state.input[n]==27&&state.input[n+1]=='['&&state.input[n+2]=='<')break;
            paste_byte(state.input[n++]);
        }
        int mouse=n<state.input_len;
        consume(n);state.raw_since=milliseconds();
        if(!mouse)return 0;
    }else if(milliseconds()-state.raw_since<25)return 0;
    state.raw_pasting=0;state.paste[state.paste_len]=0;
    return custom(e,state.paste_bad?LJ_ANSI_ERROR_CODE:LJ_ANSI_PASTE_CODE,
        state.paste_bad?"Paste rejected: exceeds 64 KiB or contains NUL":state.paste);
}
SDL_Keymod lj_ansi_modifiers(void){return state.modifiers;}
int lj_ansi_poll(SDL_Event *e){
    if(!state.opened||!e)return 0;
    if(state.has_pending){*e=state.pending;state.has_pending=0;return 1;}
    int cols,rows;dimensions(&cols,&rows);if(cols!=state.physical_cols||rows!=state.physical_rows){state.physical_cols=cols;state.physical_rows=rows;memset(e,0,sizeof *e);e->type=SDL_WINDOWEVENT;e->window.event=SDL_WINDOWEVENT_SIZE_CHANGED;e->window.data1=cols*LJ_CELL_W;e->window.data2=rows*LJ_LINE_H;return 1;}
    if(state.input_len<INPUT_SIZE){ssize_t n=read(STDIN_FILENO,state.input+state.input_len,INPUT_SIZE-state.input_len);if(n>0){state.input_len+=(size_t)n;state.input[state.input_len]=0;}}
    if(state.discarding_string){
        for(size_t i=0;i<state.input_len;i++){
            if(state.input[i]==7){consume(i+1);state.discarding_string=0;return 0;}
            if(state.input[i]==27&&i+1<state.input_len&&state.input[i+1]=='\\'){consume(i+2);state.discarding_string=0;return 0;}
        }
        size_t keep=state.input_len&&state.input[state.input_len-1]==27?1:0;
        if(state.input_len>keep)consume(state.input_len-keep);return 0;
    }
    if(state.pasting)return paste_input(e);
    if(state.raw_pasting)return raw_paste_input(e);
    if(state.message_input&&state.input_len>1&&state.input[0]!=27){
        size_t text=0;int newline=0;
        while(text<state.input_len){unsigned char c=state.input[text];
            if(c==27||(c<32&&c!='\n'&&c!='\r'&&c!='\t'))break;
            newline|=c=='\n'||c=='\r';text++;
        }
        if(text>=32||(text>1&&newline)){
            state.raw_pasting=1;state.paste_len=0;state.paste_bad=state.paste_match=0;
            return raw_paste_input(e);
        }
    }
    if(!state.input_len)return 0;
    state.modifiers=KMOD_NONE;
    if(state.input[0]==27){int result=escape(e);if(result>=0)return result;if(!state.input_len)return 0;}
    unsigned char c=state.input[0];SDL_Keymod mod=state.modifiers;
    if(c<32||c==127){consume(1);if(c==13||c==10)return key_event(e,SDLK_RETURN,mod);if(c==9)return key_event(e,SDLK_TAB,mod);if(c==127||c==8)return key_event(e,SDLK_BACKSPACE,mod);if(c==0)return key_event(e,SDLK_SPACE,(SDL_Keymod)(mod|KMOD_CTRL));if(c==29)return key_event(e,SDLK_RIGHTBRACKET,(SDL_Keymod)(mod|KMOD_CTRL));if(c>=1&&c<=26)return key_event(e,SDLK_a+c-1,(SDL_Keymod)(mod|KMOD_CTRL));return 0;}
    size_t used;uint32_t cp=decode(state.input,state.input_len,&used);if(!used)return 0;consume(used);memset(e,0,sizeof *e);e->type=SDL_TEXTINPUT;e->text.text[encode(e->text.text,cp)]=0;return 1;
}

static uint32_t scrollbar_blend(uint32_t bg,uint32_t fg,int alpha){
    uint32_t out=0;for(int shift=0;shift<=16;shift+=8)
        out|=((((bg>>shift)&255)*(255-alpha)+((fg>>shift)&255)*alpha)/255)<<shift;
    return out;
}
void lj_ansi_scrollbar(int x,int y,int h,int offset,int thumb,int opacity){
    if(!state.canvas||h<LJ_LINE_H||opacity<=0)return;
    int col=x/LJ_CELL_W,row=(y+LJ_LINE_H-1)/LJ_LINE_H;
    int rows=(y+h)/LJ_LINE_H-row;
    if(col<0||col>=state.cols||row<0||row>=state.rows||rows<1)return;
    if(row+rows>state.rows)rows=state.rows-row;
    if(opacity>255)opacity=255;
    int ph=rows*LJ_LINE_H;
    int painted=(int)((long long)thumb*ph/h);if(painted<1)painted=1;if(painted>ph)painted=ph;
    int top=h>thumb?(int)((long long)offset*(ph-painted)/(h-thumb)):0;
    if(top<0)top=0;if(top>ph-painted)top=ph-painted;
    int bottom=top+painted;
    /* Storage belongs to the queued image slot until present() finishes. */
    static uint32_t pixels[MAX_IMAGES][LJ_CELL_W*MAX_ROWS*LJ_LINE_H];
    if(lj_ansi_images_available()&&state.image_count<MAX_IMAGES){
        int slot=state.image_count;uint32_t *p=pixels[slot];
        uint64_t hash=1469598103934665603ULL;
        for(int yy=0;yy<ph;yy++)for(int xx=0;xx<LJ_CELL_W;xx++){
            uint32_t bg=state.canvas[(size_t)(row+yy/LJ_LINE_H)*state.cols+col].bg,c=bg;
            if(xx==LJ_CELL_W-3)c=scrollbar_blend(bg,lj_theme_rgb(LJ_THEME_SCROLLBAR),opacity/3);
            if(xx>=LJ_CELL_W-4&&xx<LJ_CELL_W-1&&yy>=top&&yy<bottom)c=scrollbar_blend(bg,lj_theme_rgb(LJ_THEME_SCROLLBAR),opacity);
            p[yy*LJ_CELL_W+xx]=0xff000000|c;hash=(hash^c)*1099511628211ULL;
        }
        if(lj_ansi_image(col*LJ_CELL_W,row*LJ_LINE_H,LJ_CELL_W,ph,p,LJ_CELL_W,ph)){
            /* A sampled image hash can miss a one-pixel thumb movement. */
            state.images[slot].hash=hash;return;
        }
    }
    for(int yy=0;yy<rows;yy++){
        cell *c=&state.canvas[(size_t)(row+yy)*state.cols+col];
        int on=(yy+1)*LJ_LINE_H>top&&yy*LJ_LINE_H<bottom;
        lj_ansi_glyph(col*LJ_CELL_W,(row+yy)*LJ_LINE_H,lj_theme_codepoint(on?LJ_THEME_SCROLL_THUMB:LJ_THEME_SCROLL_TRACK),
            scrollbar_blend(c->bg,lj_theme_rgb(LJ_THEME_SCROLLBAR),on?opacity:opacity/3),1);
    }
}
