/* Live drawing must not repaint the whole canvas per mouse move (the operator,
 * 2026-09-22: "lags a few seconds"; a fullscreen sixel frame costs ~130 ms).
 * Pins: the preview is drawn incrementally (only curve pieces that became
 * final, no full re-render; it trails the pen by half a segment, which the
 * released stroke completes), and lj_canvas_take_damage reports the small normalized rect that
 * changed, so the presenter can re-send only that patch. A poll/clear/resize
 * damages the whole picture. */
#define _GNU_SOURCE
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
    char root[]="/tmp/liljack-canvas-damage-XXXXXX";CHECK(mkdtemp(root));
    lj_canvas c;lj_canvas_init(&c);CHECK(lj_canvas_open(&c,root,ROOM));
    float d[4];
    CHECK(lj_canvas_render(&c,400,225,0x101010));
    CHECK(lj_canvas_take_damage(&c,d)&&d[0]==0&&d[1]==0&&d[2]==1&&d[3]==1);   /* first frame: everything */
    CHECK(!lj_canvas_take_damage(&c,d));                                        /* taken */
    float pts[]={0.10f,0.10f,0.12f,0.11f,0.14f,0.12f,0.80f,0.12f};
    lj_canvas_preview(&c,pts,2,0xffd166,4);
    CHECK(lj_canvas_take_damage(&c,d));
    CHECK(d[0]<=0.10f&&d[2]>=0.11f&&d[2]<0.14f&&d[1]<=0.10f&&d[3]<0.15f);     /* just P0 -> midpoint(P0,P1) */
    CHECK(!c.dirty);                                                            /* no full re-render queued */
    const uint32_t *before=c.pixels;
    CHECK(lj_canvas_render(&c,400,225,0x101010)==before&&!lj_canvas_take_damage(&c,d)); /* cached frame, no damage */
    CHECK(ink(c.pixels[(int)(0.1025f*225)*400+(int)(0.105f*400)],0xffd166));   /* preview is in the frame */
    lj_canvas_preview(&c,pts,3,0xffd166,4);                                     /* one more point */
    CHECK(lj_canvas_take_damage(&c,d)&&d[0]>=0.10f&&d[2]<0.17f);               /* only the new segment */
    lj_canvas_preview(&c,pts,4,0xffd166,4);                                     /* long segment to the right */
    CHECK(lj_canvas_take_damage(&c,d)&&d[0]>=0.12f&&d[2]>=0.45f&&d[2]<0.5f);   /* up to midpoint(P2,P3) */
    CHECK(ink(c.pixels[(int)(0.12f*225)*400+(int)(0.3f*400)],0xffd166));
    /* commit: append + poll re-renders the stroke smoothed; whole picture damaged */
    CHECK(lj_canvas_append_line(&c,pts,4,0xffd166,4,"operator"));
    CHECK(lj_canvas_poll(&c)==1);lj_canvas_render(&c,400,225,0x101010);
    CHECK(lj_canvas_take_damage(&c,d)&&d[0]==0&&d[2]==1);
    /* a new stroke starts from zero points already drawn */
    float p2[]={0.5f,0.5f,0.5f,0.9f};lj_canvas_preview(&c,p2,2,0x33ccff,4);
    CHECK(lj_canvas_take_damage(&c,d)&&d[0]>=0.45f&&d[1]>=0.45f&&d[3]>=0.69f&&d[3]<0.75f);   /* to midpoint(P0,P1) */
    /* resize: whole picture */
    lj_canvas_render(&c,200,112,0x101010);CHECK(lj_canvas_take_damage(&c,d)&&d[2]==1&&d[3]==1);
    lj_canvas_close(&c);
    char cmd[600];snprintf(cmd,sizeof cmd,"rm -rf -- %s",root);CHECK(system(cmd)==0);
    puts("canvas-damage: PASS");return 0;
}
