/* Fullscreen images on a wide sixel terminal (the operator, 2026-09-21: "when it's
 * full the right side is empty"). Two facts, pinned:
 *  1. the presenter REFUSES a source over IMAGE_COPY_CAP (4 MiB) outright — a
 *     2520x1060 fullscreen body on a 254-column terminal emits no DCS at all,
 *     so the terminal keeps the previous smaller image and the rest stays empty;
 *  2. the same body with its source capped at 1M pixels is emitted as ONE DCS
 *     that the VT plots across the full width (>= 6M pixels on 254x50 cells).
 * main.c's canvas blit renders at the capped size for exactly this reason. */
#define _GNU_SOURCE
#include "../liljack_app/c_ansi.h"
#include "../liljack_app/owkterm_vt.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static int read_query(int fd,char *buf,int cap,char term,int ms){int n=0;struct timespec s={0,2000000};for(int t=0;t<ms/2&&n<cap-1;t++){ssize_t z=read(fd,buf+n,(size_t)(cap-1-n));if(z>0){n+=(int)z;if(memchr(buf,term,(size_t)n))break;continue;}nanosleep(&s,NULL);}buf[n]=0;return n;}
static int drain(int fd,char *buf,int cap,int ms){int n=0;struct timespec s={0,20000000};for(int t=0;t<ms/20&&n<cap-1;t++){ssize_t z=read(fd,buf+n,(size_t)(cap-1-n));if(z>0){n+=(int)z;t=0;continue;}nanosleep(&s,NULL);}buf[n]=0;return n;}
static uint32_t nextid=1;static long plots=0;
static uint32_t tnew(void*u,uint32_t bg){(void)u;(void)bg;return nextid++;}
static void tplot(void*u,uint32_t t,int px,int py,uint32_t rgb){(void)u;(void)t;(void)px;(void)py;(void)rgb;plots++;}
static void treset(void*u,uint32_t t,uint32_t bg){(void)u;(void)t;(void)bg;}
static int run(int COLS,double cap,long *plots_out){int ROWS=58,CW=15,CH=35;plots=0;nextid=1;
    int master=posix_openpt(O_RDWR|O_NOCTTY);CHECK(master>=0&&grantpt(master)==0&&unlockpt(master)==0);
    struct winsize ws={.ws_row=(unsigned short)ROWS,.ws_col=(unsigned short)COLS,.ws_xpixel=(unsigned short)(COLS*CW),.ws_ypixel=(unsigned short)(ROWS*CH)};CHECK(ioctl(master,TIOCSWINSZ,&ws)==0);
    char *name=ptsname(master);pid_t pid=fork();CHECK(pid>=0);
    if(pid==0){close(master);int slave=open(name,O_RDWR|O_NOCTTY);if(slave<0)_exit(3);dup2(slave,0);dup2(slave,1);if(slave>2)close(slave);
        setenv("LJ_SIXEL_MIN_MS","0",1);int pw=0,ph=0;if(!lj_ansi_open(&pw,&ph))_exit(4);
        int W=pw,H=ph;                                   /* lilJack's virtual size: cols*10 x rows*20 */
        int bx=10,by=46,bw=W-20,bh=H-20-80;int sw=bw,sh=bh;
        if(cap>0&&(double)sw*sh>cap){double k=__builtin_sqrt(cap/((double)sw*sh));sw=(int)(sw*k);sh=(int)(sh*k);}
        uint32_t *src=malloc((size_t)sw*sh*4);for(long i=0;i<(long)sw*sh;i++)src[i]=0xff101418;
        for(int y=0;y<sh;y++){for(int x=0;x<20;x++)src[(long)y*sw+x]=0xff00ff00;for(int x=sw-20;x<sw;x++)src[(long)y*sw+x]=0xffff0000;}
        lj_ansi_begin(W,H);lj_ansi_rect(0,0,W,H,0x202020);
        int ok=lj_ansi_image(bx,by,bw,bh,src,sw,sh);lj_ansi_present();
        fprintf(stderr,"child: virtual %dx%d body %dx%d src %dx%d image_queued=%d avail=%d\n",W,H,bw,bh,sw,sh,ok,lj_ansi_images_available());
        lj_ansi_close();_exit(0);}
    char q[256];CHECK(read_query(master,q,sizeof q,'c',600)>0);const char *da1="\x1b[?6;4;2c";CHECK(write(master,da1,strlen(da1))>0);
    char q2[256];read_query(master,q2,sizeof q2,'t',400);char rep[64];int rn=snprintf(rep,sizeof rep,"\x1b[6;%d;%dt",CH,CW);CHECK(write(master,rep,(size_t)rn)==rn);
    static char out[1<<22];int n=drain(master,out,sizeof out,1500);int st=0;CHECK(waitpid(pid,&st,0)==pid);
    int dcs=0;for(int i=0;i+1<n;i++)if(out[i]=='\033'&&out[i+1]=='P')dcs++;
    /* find the DCS and its raster header */
    char *p=memmem(out,(size_t)n,"\033P",2);if(p){char hdr[80];memcpy(hdr,p,79);hdr[79]=0;for(int i=0;i<79;i++)if(hdr[i]=='\033')hdr[i]='~';fprintf(stderr,"first DCS header: %.79s\n",hdr);
        char *cursor=p-16;char cur[24];memcpy(cur,cursor,16);cur[16]=0;for(int i=0;i<16;i++)if(cur[i]=='\033')cur[i]='~';fprintf(stderr,"bytes before DCS: %s\n",cur);}
    void *vt=lj_vt_new(COLS,ROWS);lj_vt_pixel_sink sink={NULL,CW,CH,tnew,tplot,treset,NULL};lj_vt_set_pixel_sink(vt,&sink);
    lj_vt_feed(vt,(const unsigned char*)out,n);
    int maxcol=-1,mincol=99999,maxrow=-1;for(int r=0;r<ROWS;r++){const lj_cell *row=lj_vt_row(vt,r);for(int c=0;c<COLS;c++)if(row[c].fg&LJ_VT_FG_PIXELS){if(c>maxcol)maxcol=c;if(c<mincol)mincol=c;if(r>maxrow)maxrow=r;}}
    (void)mincol;(void)maxcol;(void)maxrow;*plots_out=plots;return dcs;}
int main(void){
    long plots=0;
    int dcs=run(254,0,&plots);CHECK(dcs==0&&plots==0);         /* fact 1: refused outright */
    puts("  fullscreen 2520x1060 source: refused by the 4 MiB cap, no DCS (the old picture would stay)");
    dcs=run(254,1000000,&plots);CHECK(dcs==1&&plots>=6000000);  /* fact 2: capped source fills the width */
    puts("  capped 1M-pixel source: one DCS, plotted across 254 columns");
    puts("canvas-fullscreen: PASS");return 0;}
