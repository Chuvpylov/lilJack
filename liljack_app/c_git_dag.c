#include "c_git_dag.h"
#include "c_render.h"
#include "c_ansi.h"
#include <stdio.h>
#include <string.h>

#define G_BG   0x0b2039u
#define G_TEXT 0xe1efffu
#define G_DIM  0x8aaed0u
#define G_GOLD 0xffd54au
#define G_WARN 0xff9a6cu
/* One colour per lane, reused round-robin. A lane keeps its colour down the
 * whole graph, which is what lets an eye follow a branch without labels. */
static const uint32_t LANE[] = {0x7fd88fu, 0x5bbcffu, 0xffd54au, 0xc99bffu, 0xff9a6cu, 0x62e0d0u};
#define NLANE ((int)(sizeof LANE / sizeof LANE[0]))

static int g_ansi;
static void grect(int x,int y,int w,int h,uint32_t c){ if(g_ansi) lj_ansi_rect(x,y,w,h,c); else lj_render_rect(x,y,w,h,c); }
static void gtext(int x,int y,const char *s,uint32_t c,int maxw){ if(!s||!*s) return; if(g_ansi) lj_ansi_text(x,y,s,c,maxw); else lj_render_text(x,y,s,c,maxw); }
static void gglyph(int x,int y,uint32_t cp,uint32_t c){ if(g_ansi) lj_ansi_glyph(x,y,cp,c,1); else lj_render_glyph(x,y,cp,c,1); }

static const char *jstr(json_object *o,const char *k){
    json_object *v=NULL;
    return (o&&json_object_object_get_ex(o,k,&v)&&json_object_is_type(v,json_type_string))?json_object_get_string(v):"";
}
static json_object *jget(json_object *o,const char *k){ json_object *v=NULL; if(o) json_object_object_get_ex(o,k,&v); return v; }
static int jint(json_object *o,const char *k){ json_object *v=jget(o,k); return (v&&json_object_is_type(v,json_type_int))?json_object_get_int(v):0; }
static int jbool(json_object *o,const char *k){ json_object *v=jget(o,k); return v&&json_object_get_boolean(v); }

int lj_git_dag_draw(json_object *dag,int x,int y,int w,int h,int scroll,int ansi){
    g_ansi=ansi;
    const int LH=LJ_LINE_H, PAD=LJ_CELL_W, RAIL=LJ_CELL_W*2;
    if(w<LJ_CELL_W*24||h<LH*2) return 0;      /* too small to draw a truthful graph */
    grect(x,y,w,h,G_BG);
    int cy=y-scroll, tx=x+PAD, inner=w-PAD*2;
    char line[400];

    const char *err=jstr(dag,"error");
    if(err&&*err){
        if(cy>=y&&cy+LH<=y+h){ snprintf(line,sizeof line,"git topology unavailable: %s",err); gtext(tx,cy,line,G_WARN,inner); }
        return LH;
    }
    json_object *rows=jget(dag,"rows");
    size_t n=rows?json_object_array_length(rows):0;
    if(!n){
        if(cy>=y&&cy+LH<=y+h) gtext(tx,cy,"no commits in range",G_DIM,inner);
        return LH;
    }
    int lanes=jint(dag,"lanes"); if(lanes<1) lanes=1; if(lanes>8) lanes=8;
    int gw=lanes*RAIL;                         /* graph gutter, then the text */

    for(size_t i=0;i<n;i++){
        json_object *r=json_object_array_get_idx(rows,i);
        int lane=jint(r,"lane"); if(lane<0) lane=0; if(lane>=lanes) lane=lanes-1;
        if(cy>=y&&cy+LH<=y+h){
            /* rails first, so a commit mark always sits ON its line */
            json_object *open=jget(r,"open");
            size_t on=open?json_object_array_length(open):0;
            for(size_t k=0;k<on;k++){
                int L=json_object_get_int(json_object_array_get_idx(open,k));
                if(L<0||L>=lanes) continue;
                grect(tx+L*RAIL+RAIL/2,cy,1,LH,LANE[L%NLANE]);
            }
            /* the commit itself: a merge is hollow so it reads differently at a glance */
            gglyph(tx+lane*RAIL+RAIL/2-LJ_CELL_W/2,cy,jbool(r,"merge")?0x25CB:0x25CF,LANE[lane%NLANE]);
            const char *refs=jstr(r,"refs");
            snprintf(line,sizeof line,"%s %s %s%s%s",jstr(r,"short"),jstr(r,"date"),
                     (refs&&*refs)?"[":"",(refs&&*refs)?refs:"",(refs&&*refs)?"] ":"");
            gtext(tx+gw,cy,line,G_DIM,inner-gw);
            int used=(int)strlen(line)*LJ_CELL_W;
            gtext(tx+gw+used,cy,jstr(r,"subject"),G_TEXT,inner-gw-used);
        }
        cy+=LH;
    }
    /* ⚠ say when the history was cut, or an empty tail reads as "no more commits" */
    if(jbool(dag,"truncated")){
        if(cy>=y&&cy+LH<=y+h){
            snprintf(line,sizeof line,"… history truncated at %zu commits",n);
            gtext(tx,cy,line,G_GOLD,inner);
        }
        cy+=LH;
    }
    return cy-(y-scroll);
}
