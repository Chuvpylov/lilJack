/* The room canvas: parse canvas.jsonl, honour clear, ignore a partial tail,
 * render lines as pixels, and pick up the operator's appended strokes on poll. */
#define _POSIX_C_SOURCE 200809L
#include "../liljack_app/c_canvas.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
/* Anti-aliased strokes: a pixel on the line is the stroke colour blended by
 * its coverage (>= 50% here), so compare channels with a tolerance. */
static int ink(uint32_t p,uint32_t c){for(int s=0;s<24;s+=8){int a=(int)((p>>s)&255),b=(int)((c>>s)&255);if(a-b>0x60||b-a>0x60)return 0;}return 1;}
static const char *ROOM="r-00000000000000000000000000000000";
int main(void){
    char root[]="/tmp/liljack-canvas-XXXXXX";CHECK(mkdtemp(root));
    lj_canvas c;lj_canvas_init(&c);CHECK(lj_canvas_open(&c,root,ROOM));
    CHECK(strstr(c.path,"/rooms/r-0000")&&strstr(c.path,"canvas.jsonl"));
    CHECK(lj_canvas_poll(&c)==0&&c.nstrokes==0);               /* no file yet is not an error */
    FILE *f=fopen(c.path,"w");CHECK(f);
    fputs("{\"t\":\"line\",\"p\":[[0,0],[1,1]],\"by\":\"codex\"}\n",f);
    fputs("{\"t\":\"clear\",\"by\":\"operator\"}\n",f);
    fputs("{\"t\":\"line\",\"c\":\"#ff0000\",\"w\":4,\"p\":[[0.0,0.5],[1.0,0.5]],\"by\":\"claude\"}\n",f);
    fputs("{\"t\":\"text\",\"p\":[[0.9,0.1]],\"s\":\"here\",\"by\":\"claude\"}\n",f);
    fputs("not json\n",f);
    fputs("{\"t\":\"line\",\"p\":[[0.5,0",f);                      /* partial tail */
    fclose(f);
    CHECK(lj_canvas_poll(&c)==1);CHECK(c.nstrokes==2);           /* clear dropped the first line */
    CHECK(c.strokes[0].kind==0&&c.strokes[0].colour==0xff0000&&c.strokes[0].npoints==2);
    CHECK(c.strokes[1].kind==1&&!strcmp(c.strokes[1].text,"here"));
    CHECK(lj_canvas_poll(&c)==0);                                 /* unchanged: no re-read */
    const uint32_t *px=lj_canvas_render(&c,100,50,0x101010);CHECK(px);
    CHECK(ink(px[25*100+50],0xff0000));                            /* on the red horizontal line */
    CHECK(px[2*100+50]==0xff101010u);                             /* background above it */
    CHECK(ink(px[5*100+90],lj_canvas_author_colour("claude")));  /* text marker */
    /* the operator drags: append, then poll sees it and the render changes. */
    float pts[]={0.5f,0.0f,0.5f,1.0f};
    CHECK(lj_canvas_append_line(&c,pts,2,0x00ff00,6,"operator"));
    CHECK(lj_canvas_poll(&c)==1);CHECK(c.nstrokes==3&&!strcmp(c.strokes[2].by,"operator"));
    px=lj_canvas_render(&c,100,50,0x101010);CHECK(ink(px[5*100+50],0x00ff00));
    lj_canvas_close(&c);
    char cmd[600];snprintf(cmd,sizeof cmd,"rm -rf -- %s",root);CHECK(system(cmd)==0);
    puts("canvas-render: PASS");return 0;
}
