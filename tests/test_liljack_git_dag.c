/* c_git_dag: the commit DAG renderer.
 *
 * Properties, not pixels: it must report content height, clip to its box on
 * EVERY path, say why it is empty rather than drawing nothing, and DRAW the
 * truncation — a graph that silently stops reads as a repo with no older
 * history, which is a lie about the data.
 */
#include "c_git_dag.h"
#include "c_render.h"
#include <stdio.h>
#include <string.h>
#define W 800
#define H 400
static int ok=0, fail=0;
static void check(int c,const char *n){ if(c){ok++;printf("  ok     %s\n",n);} else {fail++;printf("  FAIL   %s\n",n);} }
static void clear(void){ lj_render_rect(0,0,W,H,0x000000u); }
static long ink(void){ uint32_t *p=lj_render_pixels(); long k=0; for(size_t i=0;i<(size_t)W*H;i++) if((p[i]&0xffffff)>0x0c2340u) k++; return k; }

static json_object *row(const char *sha,int lane,int merge,const int *open,int n){
    json_object *r=json_object_new_object();
    json_object_object_add(r,"short",json_object_new_string(sha));
    json_object_object_add(r,"date",json_object_new_string("2026-09-09"));
    json_object_object_add(r,"subject",json_object_new_string("a commit subject"));
    json_object_object_add(r,"refs",json_object_new_string(""));
    json_object_object_add(r,"lane",json_object_new_int(lane));
    json_object_object_add(r,"merge",json_object_new_boolean(merge));
    json_object *o=json_object_new_array();
    for(int i=0;i<n;i++) json_object_array_add(o,json_object_new_int(open[i]));
    json_object_object_add(r,"open",o);
    return r;
}
static json_object *dag(int lanes,int truncated,int nrows){
    json_object *d=json_object_new_object(), *rows=json_object_new_array();
    int open2[2]={0,1};
    for(int i=0;i<nrows;i++) json_object_array_add(rows,row("abc1234",i%lanes,i%3==0,open2,lanes>1?2:1));
    json_object_object_add(d,"rows",rows);
    json_object_object_add(d,"lanes",json_object_new_int(lanes));
    json_object_object_add(d,"truncated",json_object_new_boolean(truncated));
    json_object_object_add(d,"error",NULL);
    return d;
}

int main(int argc,char **argv){
    if(lj_render_init(W,H)<0){ puts("no faces"); return 2; }

    json_object *d=dag(1,0,6);
    clear(); int hgt=lj_git_dag_draw(d,0,0,W,H,0,0);
    check(hgt>0 && ink()>0,"draws a linear history and reports its height");
    clear(); check(lj_git_dag_draw(d,0,0,W,H,40,0)==hgt,"content height is independent of scroll");

    json_object *multi=dag(3,0,6);
    clear(); lj_git_dag_draw(multi,0,0,W,H,0,0); long a=ink();
    clear(); lj_git_dag_draw(d,0,0,W,H,0,0); long b=ink();
    check(a>b,"three lanes draw more rail than one — branches are visible as branches");

    json_object *tr=dag(1,1,4);
    clear(); int th=lj_git_dag_draw(tr,0,0,W,H,0,0);
    clear(); int nh=lj_git_dag_draw(dag(1,0,4),0,0,W,H,0,0);
    check(th>nh,"a truncated history DRAWS the truncation instead of just stopping");

    json_object *err=json_object_new_object();
    json_object_object_add(err,"error",json_object_new_string("not a git repository"));
    clear(); check(lj_git_dag_draw(err,0,0,W,H,0,0)>0 && ink()>0,"an unavailable topology states its reason");

    json_object *empty=json_object_new_object();
    json_object_object_add(empty,"rows",json_object_new_array());
    clear(); check(lj_git_dag_draw(empty,0,0,W,H,0,0)>0 && ink()>0,"an empty range says so rather than drawing nothing");

    check(lj_git_dag_draw(d,0,0,40,10,0,0)==0,"a box too small for a truthful graph draws nothing");

    /* clipping on every path, including the error and empty branches */
    {
        const int bx=120,by=90,bw=320,bh=120; int leaked=0;
        json_object *cases[4]={d,err,empty,tr};
        for(int c=0;c<4;c++) for(int sc=0;sc<=60;sc+=60){
            clear(); lj_git_dag_draw(cases[c],bx,by,bw,bh,sc,0);
            uint32_t *px=lj_render_pixels();
            for(int yy=0;yy<H;yy++) for(int xx=0;xx<W;xx++){
                int in = xx>=bx&&xx<bx+bw&&yy>=by&&yy<by+bh;
                if(!in&&(px[(size_t)yy*W+xx]&0xffffff)>0x0c2340u) leaked=1;
            }
        }
        check(!leaked,"NOTHING is drawn outside the tile, on every path and at every scroll");
    }

    lj_render_close();
    printf("\n%d checks passed, %d failed\n",ok,fail);
    return fail?1:0;
}
