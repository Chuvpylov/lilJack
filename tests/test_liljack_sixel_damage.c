/* Live drawing lag (the operator 2026-09-22: "still laggy, a few seconds").
 * Measured: a fullscreen canvas frame is ~140 KB of sixel and ~130 ms of
 * encode + owkTerm decode, re-sent whole on every pen move, and faster than
 * the terminal can take it. Pinned here, on a fake pty acting as a SLOW
 * terminal:
 *  1. frame 1 (new image) is emitted whole and followed by a DA1 query (ESC[c)
 *     that works as a frame ACK;
 *  2. while that ACK is unanswered, frame 2's content change is HELD (no DCS),
 *     so a slow terminal can never accumulate a backlog;
 *  3. after the ACK, the held change goes out as ONE small patch (damaged
 *     cells only, placed with CUP at the damaged cell), not the whole image;
 *  4. a damage rect forces the patch even when the sampled hash cannot see a
 *     thin new line. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../liljack_app/c_ansi.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
static int read_until(int fd,char *buf,int cap,int have,const char *needle,int ms){
    struct timespec s={0,2000000};
    for(int t=0;t<ms/2&&have<cap-1;t++){buf[have]=0;if(memmem(buf,(size_t)have,needle,strlen(needle)))return have;
        ssize_t z=read(fd,buf+have,(size_t)(cap-1-have));if(z>0){have+=(int)z;t=0;continue;}nanosleep(&s,NULL);}
    buf[have]=0;return have;}
static int count_dcs(const char *p,const char *end){int n=0;for(;p+1<end;p++)if(p[0]=='\033'&&p[1]=='P')n++;return n;}
static long dcs_bytes(const char *p,const char *end){long n=0;int in=0;for(;p+1<end;p++){if(p[0]=='\033'&&p[1]=='P')in=1;if(in)n++;if(p[0]=='\033'&&p[1]=='\\')in=0;}return n;}
enum{W=1500,H=700,SW=750,SH=350};   /* body in lilJack virtual px; source at half res */
int main(void){
    int COLS=160,ROWS=40,CW=15,CH=35;
    int master=posix_openpt(O_RDWR|O_NOCTTY);CHECK(master>=0&&grantpt(master)==0&&unlockpt(master)==0);
    struct winsize ws={.ws_row=(unsigned short)ROWS,.ws_col=(unsigned short)COLS,.ws_xpixel=(unsigned short)(COLS*CW),.ws_ypixel=(unsigned short)(ROWS*CH)};CHECK(ioctl(master,TIOCSWINSZ,&ws)==0);
    char *name=ptsname(master);pid_t pid=fork();CHECK(pid>=0);
    if(pid==0){close(master);int slave=open(name,O_RDWR|O_NOCTTY);if(slave<0)_exit(3);dup2(slave,0);dup2(slave,1);if(slave>2)close(slave);
        setenv("LJ_SIXEL_MIN_MS","0",1);unsetenv("OWKTERM");int pw=0,ph=0;if(!lj_ansi_open(&pw,&ph))_exit(4);
        /* a busy picture (strokes everywhere), so sixel size follows area as on a real canvas */
        uint32_t *src=malloc((size_t)SW*SH*4);for(long i=0;i<(long)SW*SH;i++)src[i]=((i%SW)/3+(i/SW)/5)%2?0xff101418:0xff33ccff;
        /* frame 1: the whole canvas */
        lj_ansi_begin(pw,ph);lj_ansi_rect(0,0,pw,ph,0x202020);CHECK(lj_ansi_image(0,0,W,H,src,SW,SH));lj_ansi_present();
        if(write(1,"@@F2@@",6)!=6)_exit(5);
        /* frame 2: one thin pen segment (4 source px) far from any hash sample density */
        for(int y=200;y<204;y++)for(int x=500;x<540;x++)src[(long)y*SW+x]=0xffffd166;
        lj_ansi_begin(pw,ph);lj_ansi_rect(0,0,pw,ph,0x202020);CHECK(lj_ansi_image(0,0,W,H,src,SW,SH));
        lj_ansi_image_damage(500,200,40,4);lj_ansi_present();
        if(write(1,"@@W@@",5)!=5)_exit(6);
        /* wait for the terminal's ACK to arrive through the normal input path */
        for(int t=0;t<300;t++){SDL_Event e;lj_ansi_poll(&e);struct timespec s={0,2000000};nanosleep(&s,NULL);}
        if(write(1,"@@F3@@",6)!=6)_exit(7);
        lj_ansi_begin(pw,ph);lj_ansi_rect(0,0,pw,ph,0x202020);CHECK(lj_ansi_image(0,0,W,H,src,SW,SH));lj_ansi_present();
        if(write(1,"@@END@@",7)!=7)_exit(8);
        lj_ansi_close();_exit(0);}
    static char out[1<<22];int n=0;
    n=read_until(master,out,sizeof out,n,"\033[c",1500);            /* open-time DA1 probe */
    CHECK(write(master,"\033[?6;4;2c",9)==9);
    n=read_until(master,out,sizeof out,n,"\033[16t",600);            /* cell-size probe */
    if(memmem(out,(size_t)n,"\033[16t",5)){char rep[32];int rn=snprintf(rep,sizeof rep,"\033[6;%d;%dt",CH,CW);CHECK(write(master,rep,(size_t)rn)==rn);}
    n=read_until(master,out,sizeof out,n,"@@W@@",3000);
    char *f2=memmem(out,(size_t)n,"@@F2@@",6),*w=memmem(out,(size_t)n,"@@W@@",5);CHECK(f2&&w);
    int f1_dcs=count_dcs(out,f2);long f1_bytes=dcs_bytes(out,f2);
    CHECK(f1_dcs==1);                                                 /* 1: whole image once */
    CHECK(memmem(f2-64>out?f2-64:out,(size_t)(f2-(f2-64>out?f2-64:out)),"\033[c",3));     /* ...followed by the ACK query */
    CHECK(count_dcs(f2,w)==0);                                        /* 2: held while unacknowledged */
    CHECK(write(master,"\033[?6;4;2c",9)==9);                         /* the terminal catches up */
    n=read_until(master,out,sizeof out,n,"@@END@@",3000);
    char *f3=memmem(out,(size_t)n,"@@F3@@",6),*end=memmem(out,(size_t)n,"@@END@@",7);CHECK(f3&&end);
    int f3_dcs=count_dcs(f3,end);long f3_bytes=dcs_bytes(f3,end);
    fprintf(stderr,"  frame1 %ld bytes of sixel, frame3 patch %ld bytes\n",f1_bytes,f3_bytes);
    CHECK(f3_dcs==1);                                                 /* 3: the held change, once */
    CHECK(f3_bytes*8<f1_bytes);                                       /*    as a patch, not the whole */
    /* placed at the damaged cells: source x 500/750 of 150 cols -> col 100, y 200/350 of 35 rows -> row 20 */
    char *dcs=memmem(f3,(size_t)(end-f3),"\033P",2);CHECK(dcs);
    char *cup=NULL;for(char *p=f3;p<dcs;p++)if(p[0]=='\033'&&p[1]=='['&&memchr(p,'H',(size_t)(dcs-p)))cup=p;
    CHECK(cup);int r=0,c=0;CHECK(sscanf(cup,"\033[%d;%dH",&r,&c)==2);
    fprintf(stderr,"  patch placed at row %d col %d\n",r,c);
    CHECK(r>=19&&r<=21&&c>=98&&c<=101);
    int st=0;CHECK(waitpid(pid,&st,0)==pid&&WIFEXITED(st)&&WEXITSTATUS(st)==0);
    puts("sixel-damage: PASS");return 0;
}
