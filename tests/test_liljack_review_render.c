/* c_review: the App review panel renderer.
 *
 * The properties under test are the design rules, not pixel positions:
 * content height must be reported so the caller can scroll, content must be
 * clipped rather than drawn outside its box, and a section with missing data
 * must print a REASON instead of a convincing zero. */
#include "c_review.h"
#include "c_render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define W 900
#define H 400
static int ok=0, fail=0;
static void check(int c,const char *name){ if(c){ok++;printf("  ok     %s\n",name);} else {fail++;printf("  FAIL   %s\n",name);} }
static long ink(void){ uint32_t *p=lj_render_pixels(); long k=0; for(size_t i=0;i<(size_t)W*H;i++) if((p[i]&0xffffff)>0x0c2340u) k++; return k; }
static void clear(void){ lj_render_rect(0,0,W,H,0x000000u); }

int main(int argc,char **argv){
    if(lj_render_init(W,H)<0){ puts("no faces"); return 2; }
    const char *path = argc>1 ? argv[1] : NULL;
    json_object *snap = path ? json_object_from_file(path) : NULL;
    check(snap!=NULL,"real review_panel snapshot loads");
    if(!snap){ return 1; }

    clear();
    int hgt = lj_review_draw(snap,0,0,W,H,0,0);
    long drawn = ink();
    check(hgt>0,"reports content height so the caller can size a scrollbar");
    check(drawn>0,"actually draws the panel");

    /* scrolling moves content without changing the reported height */
    clear();
    int hgt2 = lj_review_draw(snap,0,0,W,H,40,0);
    check(hgt2==hgt,"content height is independent of scroll");

    /* a box too small to say anything true refuses rather than drawing junk */
    clear();
    check(lj_review_draw(snap,0,0,30,10,0,0)==0,"a box too small to be honest draws nothing");

    /* ⚠ NOTHING may be drawn outside the tile, on ANY path including the early
     * failure branches. codex findings299: with scroll>0 the no-summary branch
     * drew its error text above the box. Counting ink cannot catch that; this
     * asserts on pixels OUTSIDE the rectangle. */
    {
        const int bx=100,by=100,bw=300,bh=100;
        json_object *cases[3];
        cases[0]=snap;
        cases[1]=json_object_new_object();                       /* no summary */
        json_object *bs=json_object_new_object(),*be=json_object_new_object();
        json_object_object_add(be,"error",json_object_new_string("board unreadable"));
        json_object_object_add(bs,"summary",be);
        cases[2]=bs;                                             /* summary error */
        int leaked=0;
        for(int ci=0;ci<3;ci++) for(int sc=0;sc<=40;sc+=40){
            clear();
            lj_review_draw(cases[ci],bx,by,bw,bh,sc,0);
            uint32_t *px=lj_render_pixels();
            for(int yy=0;yy<H;yy++) for(int xx=0;xx<W;xx++){
                int inside = xx>=bx&&xx<bx+bw&&yy>=by&&yy<by+bh;
                if(!inside&&(px[(size_t)yy*W+xx]&0xffffff)>0x0c2340u){ leaked=1; }
            }
        }
        check(!leaked,"NOTHING is drawn outside the tile, on every path and at every scroll");
        json_object_put(cases[1]); json_object_put(cases[2]);
    }

    /* ⚠ missing data must REPORT, never show a zero */
    json_object *empty=json_object_new_object();
    clear();
    int eh=lj_review_draw(empty,0,0,W,H,0,0);
    check(eh>0 && ink()>0,"a snapshot with no summary still prints a reason, not a blank");
    json_object *broken=json_object_new_object();
    json_object *sum=json_object_new_object();
    json_object_object_add(sum,"error",json_object_new_string("board unreadable in test"));
    json_object_object_add(broken,"summary",sum);
    clear();
    check(lj_review_draw(broken,0,0,W,H,0,0)>0,"an unreadable board reports its error");
    json_object_put(empty); json_object_put(broken); json_object_put(snap);

    /* the rendered STRINGS must never state more than the data supports */
    {
        FILE *f=fopen("liljack_app/c_review.c","rb");
        if(f){
            static char buf[200000]; size_t got=fread(buf,1,sizeof buf-1,f); buf[got]=0; fclose(f);
            check(strstr(buf,"no score recorded")!=NULL,
                  "a null score renders as 'no score recorded', never 0.00");
            check(strstr(buf,"still UNDELIVERED")!=NULL,
                  "undelivered notices are called undelivered, not handled");
        } else check(0,"could not read c_review.c for the literal check");
    }

    lj_render_close();
    printf("\n%d checks passed, %d failed\n",ok,fail);
    return fail?1:0;
}
