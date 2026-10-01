#include "c_dash.h"
#include "c_render.h"
#include "c_ansi.h"
#include <stdio.h>
#include <string.h>

#define D_BG    0x0b2039u
#define D_EDGE  0x214f7au
#define D_TEXT  0xe1efffu
#define D_DIM   0x8aaed0u
#define D_GOLD  0xffd54au
#define D_CYAN  0x5bbcffu
#define D_OK    0x7fd88fu
#define D_WARN  0xff9a6cu

/* Column plan, in cells: agent 9 + gap, role 8 + gap, state 8 + gap = 28, then
 * the host's own field, and only then the task. Keeping these as named
 * constants is what stops the next edit from overwriting a neighbour. */
#define D_HOST_CELLS 10
#define D_TASK_COL   (28 + D_HOST_CELLS + 1)

static int g_ansi;
static void drect(int x,int y,int w,int h,uint32_t c){ if(w<=0||h<=0)return; if(g_ansi) lj_ansi_rect(x,y,w,h,c); else lj_render_rect(x,y,w,h,c); }
static void dtext(int x,int y,const char *s,uint32_t c,int maxw){ if(!s||!*s||maxw<=0) return; if(g_ansi) lj_ansi_text(x,y,s,c,maxw); else lj_render_text(x,y,s,c,maxw); }

static const char *jstr(json_object *o,const char *k){
    json_object *v=NULL;
    return (o&&json_object_object_get_ex(o,k,&v)&&json_object_is_type(v,json_type_string))?json_object_get_string(v):"";
}
static json_object *jget(json_object *o,const char *k){
    json_object *v=NULL; return (o&&json_object_object_get_ex(o,k,&v))?v:NULL;
}
static int jint(json_object *o,const char *k,int missing){
    json_object *v=jget(o,k);
    return (v&&json_object_is_type(v,json_type_int))?json_object_get_int(v):missing;
}
static int arr(json_object *o,const char *k,json_object **out){
    json_object *v=jget(o,k);
    if(!v||!json_object_is_type(v,json_type_array)){ *out=NULL; return 0; }
    *out=v; return (int)json_object_array_length(v);
}

/* "3/7" -> 3 and 7. Returns 0 when the string is not a ratio, so a task with a
 * free-text progress note ("holding for review") draws its text and NO bar
 * rather than a bar at zero. */
static int ratio(const char *s,int *done,int *total){
    if(!s||!*s) return 0;
    int a=0,b=0; char extra=0;
    if(sscanf(s,"%d/%d%c",&a,&b,&extra)<2) return 0;
    if(b<=0||a<0) return 0;
    *done=a>b?b:a; *total=b; return 1;
}

static uint32_t state_colour(const char *state){
    if(!strcmp(state,"running")||!strcmp(state,"active")) return D_OK;
    if(!strcmp(state,"busy")) return D_CYAN;
    if(!strcmp(state,"blocked")) return D_WARN;
    return D_DIM;
}

int lj_dash_draw(json_object *dash,int x,int y,int w,int h,int scroll,int ansi){
    g_ansi=ansi;
    int CW,LH;
    if(ansi){ lj_ansi_cell_pixels(&CW,&LH); } else { CW=LJ_CELL_W; LH=LJ_LINE_H; }
    if(CW<1)CW=LJ_CELL_W; if(LH<1)LH=LJ_LINE_H;
    int cy=y-scroll, left=x+CW, right=x+w-CW, avail=right-left;
    if(avail<8*CW) avail=8*CW;

    if(!dash){
        dtext(left,cy,"Dashboard: no snapshot yet",D_DIM,avail);
        return LH;
    }
    char line[512];
    json_object *totals=jget(dash,"totals");
    const char *host=jstr(dash,"host");
    json_object *git=jget(dash,"git");
    const char *branch=jstr(git,"branch");
    int rows=jint(git,"rows",-1);

    /* ── header: the one-line answer ─────────────────────────────────────── */
    snprintf(line,sizeof line,"lilJack · %d agents · %d open of %d tasks · %s",
             jint(totals,"agents",0),jint(totals,"open",0),jint(totals,"tasks",0),
             host&&*host?host:"this host");
    dtext(left,cy,line,D_GOLD,avail); cy+=LH;
    if(branch&&*branch){
        if(rows>=0) snprintf(line,sizeof line,"git %s · %d commits shown",branch,rows);
        else        snprintf(line,sizeof line,"git %s · commits —",branch);   /* never 0 */
    } else snprintf(line,sizeof line,"git —");
    dtext(left,cy,line,D_DIM,avail); cy+=LH;
    const char *err=jstr(dash,"error");
    if(err&&*err){ snprintf(line,sizeof line,"review: %s",err); dtext(left,cy,line,D_WARN,avail); cy+=LH; }
    cy+=LH;

    /* ── agents ──────────────────────────────────────────────────────────── */
    json_object *agents=NULL; int n=arr(dash,"agents",&agents);
    dtext(left,cy,n?"AGENTS":"AGENTS · none live",D_CYAN,avail); cy+=LH;
    for(int i=0;i<n;i++){
        json_object *ag=json_object_array_get_idx(agents,i);
        const char *name=jstr(ag,"agent"), *role=jstr(ag,"role"), *st=jstr(ag,"state");
        const char *ahost=jstr(ag,"host"), *task=jstr(ag,"task");
        json_object *un=jget(ag,"unavailable");
        const char *reason=(un&&json_object_is_type(un,json_type_string))?json_object_get_string(un):NULL;
        /* ⚠ COLUMNS MUST NOT WRITE OVER EACH OTHER. The identity line is
         * printed with %-9s %-8s %-8s %s, which is 9+1+8+1+8+1 = 28 cells
         * BEFORE the host — so drawing the task at column 28 painted over the
         * host and "devbox" came out as "—ibika" (seen in the first capture,
         * 2026-09-12). The task column starts after the host's own field. */
        snprintf(line,sizeof line,"%-9s %-8s %-8s %-*s",
                 name&&*name?name:"agent", role&&*role?role:"worker",
                 st&&*st?st:"unknown", D_HOST_CELLS, ahost&&*ahost?ahost:"—");
        dtext(left,cy,line,state_colour(st?st:""),avail);
        int taskx=left+D_TASK_COL*CW, taskw=avail-D_TASK_COL*CW;
        if(reason&&*reason){
            snprintf(line,sizeof line,"unavailable: %s",reason);
            dtext(taskx,cy,line,D_WARN,taskw);
        } else if(task&&*task){
            dtext(taskx,cy,task,D_TEXT,taskw);
        } else {
            dtext(taskx,cy,"—",D_DIM,taskw);   /* idle is not "nothing measured" */
        }
        cy+=LH;
    }
    cy+=LH;

    /* ── work, with a bar only where a ratio was actually reported ───────── */
    json_object *work=NULL; int m=arr(dash,"work",&work);
    dtext(left,cy,m?"WORK":"WORK · nothing open",D_CYAN,avail); cy+=LH;
    for(int i=0;i<m;i++){
        json_object *t=json_object_array_get_idx(work,i);
        const char *task=jstr(t,"task"), *who=jstr(t,"agent"), *st=jstr(t,"state");
        const char *prog=jstr(t,"progress");
        json_object *known=jget(t,"known_agent");
        int live=known&&json_object_is_type(known,json_type_boolean)&&json_object_get_boolean(known);
        snprintf(line,sizeof line,"%-8s %-9s %s",st&&*st?st:"?",who&&*who?who:"—",task&&*task?task:"—");
        dtext(left,cy,line,state_colour(st?st:""),avail-14*CW);
        /* The ratio is written BESIDE its bar, never on top of it: text over a
         * filled bar is unreadable at either end of the fill. */
        int bx=right-10*CW, labelx=right-16*CW, done=0,total=0;
        if(ratio(prog,&done,&total)){
            int barw=10*CW, fill=total?barw*done/total:0;
            drect(bx,cy+LH/4,barw,LH/2,D_EDGE);
            drect(bx,cy+LH/4,fill,LH/2,D_OK);
            dtext(labelx,cy,prog,D_DIM,6*CW);
        } else if(prog&&*prog){
            dtext(labelx,cy,prog,D_DIM,16*CW);      /* free text, no bar */
        }
        if(!live){ dtext(right-2*CW,cy,"·",D_WARN,2*CW); }   /* owner session gone */
        cy+=LH;
    }
    cy+=LH;

    /* ── hosts ───────────────────────────────────────────────────────────── */
    json_object *hosts=NULL; int k=arr(dash,"hosts",&hosts);
    if(k){
        size_t at=(size_t)snprintf(line,sizeof line,"HOSTS ");
        for(int i=0;i<k&&at+2<sizeof line;i++){
            const char *hv=json_object_get_string(json_object_array_get_idx(hosts,i));
            at+=(size_t)snprintf(line+at,sizeof line-at,"%s%s",i?" · ":"",hv?hv:"");
        }
        dtext(left,cy,line,D_DIM,avail); cy+=LH;
    }
    return cy+scroll-y;
}
