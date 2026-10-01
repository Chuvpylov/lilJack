#define _POSIX_C_SOURCE 200809L
/* Presenter-only composition probe. No backend, sessions, files or live data. */
#include "../liljack_app/c_ansi.h"
#include "../liljack_app/c_theme.h"
#include <hui_border.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <locale.h>
#define CW 10
#define CH 20
static volatile sig_atomic_t running=1;
static void stop(int s){(void)s;running=0;}
static unsigned long ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (unsigned long)t.tv_sec*1000+t.tv_nsec/1000000;}
static int bounded(const char *s,int lo,int hi){char *end;long n=strtol(s,&end,10);if(*end||n<lo||n>hi){fprintf(stderr,"invalid value: %s\n",s);exit(2);}return (int)n;}
/* Row-major inventory matches main.c --gallery. The outer lead ring and four
 * graph images above are the full-size instances; these are compact states.
 * No labels are painted: keep the inventory in the evidence metadata. */
static void gallery(int w,int top,int h,unsigned elapsed){
    const uint32_t palette[]={lj_theme_rgb(LJ_THEME_LAB_BG),lj_theme_rgb(LJ_THEME_LAB_PANEL),lj_theme_rgb(LJ_THEME_LAB_EDGE),lj_theme_rgb(LJ_THEME_LAB_SURF2),
        lj_theme_rgb(LJ_THEME_LAB_SURF3),lj_theme_rgb(LJ_THEME_LAB_TEXT),lj_theme_rgb(LJ_THEME_LAB_DIM),lj_theme_rgb(LJ_THEME_LAB_BLUE),lj_theme_rgb(LJ_THEME_LAB_GOLD),lj_theme_rgb(LJ_THEME_LAB_RED),lj_theme_rgb(LJ_THEME_LAB_GREEN)};
    int slot=(w/CW/2-4)/3,available=h/CH-3;
    int stride=available/6;if(stride<1)stride=1;
    for(int i=0;i<36;i++){
        int col=i%6,row=i/6;
        int x=(col<3?2: w/CW/2+2)*CW+(col%3)*slot*CW;
        int y=top+(2+row*stride)*CH,bw=(slot-1)*CW;if(bw<CW)continue;
        if(y+CH>=h+top)continue;
        uint32_t fg=palette[5],bg=palette[1];const char *glyph="  ";
        if(i<11)bg=palette[i];
        else switch(i){
            case 11:glyph="┌─┐";fg=palette[2];break;
            case 12:glyph="• •";fg=palette[8+(elapsed/500)%2];break;
            case 13:bg=palette[7];break;
            case 14:glyph="───";break;
            case 15:glyph="│";break;
            case 16:glyph="─┬─";break;
            case 17:bg=palette[3];break;
            case 18:bg=palette[4];break;
            case 19:bg=palette[10];break;
            case 20:fg=palette[6];glyph="·";break;
            case 21:glyph="▔ ▔";bg=palette[3];break;
            case 22:glyph="●";fg=palette[7];break;
            case 23:glyph="+";bg=palette[3];break;
            case 24:glyph="●";fg=palette[10];break;
            case 25:glyph="◐";fg=palette[8];break;
            case 26:glyph="○";fg=palette[9];break;
            case 27:glyph="▁▃▆";fg=palette[10];break;
            case 28:glyph="▂▅▇";fg=palette[7];break;
            case 29:glyph="▁▂▃▄▅▆▇█";fg=palette[10];break;
            case 30:glyph="✓ ✻ ✽";break;
            case 31:glyph="🛠 中";fg=palette[7];break;
            case 32:{static const char *s[]={"⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧"};glyph=s[(elapsed/120)%8];break;}
            case 33:glyph="┌ ┐";bg=palette[4];break;
            case 34:glyph="✓";bg=palette[10];break;
            case 35:glyph="▐";fg=palette[6];break;
        }
        lj_ansi_rect(x,y,bw,CH,bg);lj_ansi_text(x,y,glyph,fg,bw);
    }
}
/* Same contained-cell anchors and inset sixel rect as main.c:dotted_border. */
static void ring(int x,int y,int w,int h,unsigned elapsed,int placeholders,int tick){
    hui_border_grid g;if(!hui_border_cells(x,y,w,h,CW,CH,&g))return;
    if(placeholders)for(int edge=0;edge<4;edge++){
        int count=hui_border_edge_count((edge&1)?g.rows:g.cols);
        for(int i=0;i<count-1;i++){
            hui_border_point p;if(!hui_border_anchor(g.cols,g.rows,edge,i,&p))continue;
            uint32_t color=hui_border_color_index(p.ordinal,(elapsed/120)/4)?lj_theme_rgb(LJ_THEME_LAB_BLUE):lj_theme_rgb(LJ_THEME_LAB_GOLD);
            int drawn=lj_ansi_dot((g.col+p.col)*CW,(g.row+p.row)*CH,color);
            if(!drawn&&(edge&1)&&count==3&&i==1)for(int a=0;a<g.rows;a++){
                int row=hui_border_interior_candidate(g.rows,a);
                if(row>=0&&lj_ansi_dot((g.col+p.col)*CW,(g.row+row)*CH,color))break;
            }
        }
    }
    lj_ansi_border(x+CW,y+CH,w-2*CW,h-2*CH,lj_theme_rgb(LJ_THEME_LAB_GOLD),lj_theme_rgb(LJ_THEME_LAB_BLUE),elapsed/(unsigned)tick);
}
int main(int argc,char **argv){
    int seconds=30,tick=50,cols=0,rows=0,graphs=1,lead=1,placeholders=1;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--help")){puts("ansi_lab [--seconds N] [--tick-ms N] [--size COLSxROWS] [--no-ring] [--no-graphs] [--no-placeholders]\nRuns in this terminal; Ctrl+Q or Escape exits. Synthetic pixels only.");return 0;}
        else if(!strcmp(argv[i],"--seconds")&&i+1<argc)seconds=bounded(argv[++i],1,3600);
        else if(!strcmp(argv[i],"--tick-ms")&&i+1<argc)tick=bounded(argv[++i],10,1000);
        else if(!strcmp(argv[i],"--size")&&i+1<argc){char tail;if(sscanf(argv[++i],"%dx%d%c",&cols,&rows,&tail)!=2||cols<30||cols>300||rows<12||rows>120)return 2;}
        else if(!strcmp(argv[i],"--no-ring"))lead=0;
        else if(!strcmp(argv[i],"--no-graphs"))graphs=0;
        else if(!strcmp(argv[i],"--no-placeholders"))placeholders=0;
        else return 2;
    }
    setlocale(LC_CTYPE,"");signal(SIGTERM,stop);signal(SIGINT,stop);
    char error[256];if(lj_theme_load_default(error,sizeof error)<0)fprintf(stderr,"Theme: %s\n",error);
    int w,h;if(!lj_ansi_open(&w,&h))return 1;
    if(cols)w=cols*CW;if(rows)h=rows*CH;
    if(w<30*CW||h<12*CH){lj_ansi_close();fprintf(stderr,"lab needs at least 30x12 cells\n");return 2;}
    int cellw,cellh;lj_ansi_cell_pixels(&cellw,&cellh);
    int half=(w/CW/2)*CW,top=4*CH,tileh=h-top-CH;
    fprintf(stderr,"ansi-lab cols=%d rows=%d cell=%dx%d lead=%d,%d,%d,%d graphs=%d placeholders=%d tick_ms=%d\n",w/CW,h/CH,cellw,cellh,CW,top,half-2*CW,tileh,graphs,placeholders,tick);
    unsigned long start=ms(),last=0;unsigned frames=0;
    while(running&&ms()-start<(unsigned long)seconds*1000){
        unsigned elapsed=(unsigned)(ms()-start);
        SDL_Event e;while(lj_ansi_poll(&e)){
            if(e.type==SDL_KEYDOWN&&(e.key.keysym.sym==SDLK_ESCAPE||((e.key.keysym.mod&KMOD_CTRL)&&e.key.keysym.sym==SDLK_q)))running=0;
            if(e.type==SDL_USEREVENT)free(e.user.data1);
        }
        if(!frames||elapsed-last>=100){
            lj_ansi_begin(w,h);lj_ansi_rect(0,0,w,h,lj_theme_rgb(LJ_THEME_LAB_BG));
            if(graphs){
                int gw=(w/CW/4-2)*CW,gh=2*CH;
                uint32_t *pixels=malloc((size_t)gw*gh*sizeof *pixels);if(!pixels){running=0;break;}
                for(int k=0;k<4;k++){
                    for(int i=0;i<gw*gh;i++)pixels[i]=(0xff000000u|lj_theme_rgb(LJ_THEME_LAB_GRAPH_BG));
                    for(int x=0;x<gw;x++){
                        float v=.50f+.32f*sinf((x+(int)frames)*.11f+k)+.08f*sinf((x+(int)frames)*.37f);
                        int y=gh-2-(int)(v*(gh-4));for(int dy=0;dy<2;dy++)pixels[(y+dy)*gw+x]=(0xff000000u|lj_theme_rgb(LJ_THEME_LAB_GREEN));
                    }
                    lj_ansi_image((k*(gw/CW+2)+1)*CW,CH,gw,gh,pixels,gw,gh);
                }free(pixels);
            }
            lj_ansi_rect(2*CW,top+CH,half-4*CW,CH,lj_theme_rgb(LJ_THEME_LAB_SURF2));
            lj_ansi_text(3*CW,top+CH,"   ",lj_theme_rgb(LJ_THEME_LAB_WHITE),3*CW);
            lj_ansi_rect(half+2*CW,top+CH,w-half-4*CW,CH,lj_theme_rgb(LJ_THEME_LAB_SURF2));
            lj_ansi_rect(half+CW,h-2*CH,w-half-2*CW,1,lj_theme_rgb(LJ_THEME_LAB_HAIRLINE));
            lj_ansi_separator(half,top,CW,tileh,lj_theme_rgb(LJ_THEME_LAB_GOLD),1);
            lj_ansi_separator(half+CW,top+tileh/2,w-half-2*CW,CH,lj_theme_rgb(LJ_THEME_LAB_GOLD),0);
            gallery(w,top,tileh,elapsed);
            if(lead)ring(CW,top,half-2*CW,tileh,elapsed,placeholders,tick);
            if(!lj_ansi_present()){running=0;break;}last=elapsed;frames++;
        }
        if(!lj_ansi_border_tick(elapsed/(unsigned)tick)){running=0;break;}
        struct timespec pause={0,4000000};nanosleep(&pause,NULL);
    }
    lj_ansi_close();fprintf(stderr,"ansi-lab frames=%u elapsed_ms=%lu\n",frames,ms()-start);return 0;
}
