#include "c_review.h"
#include "c_render.h"
#include "c_ansi.h"
#include <stdio.h>
#include <string.h>

#define R_BG    0x0b2039u
#define R_EDGE  0x214f7au
#define R_TEXT  0xe1efffu
#define R_DIM   0x8aaed0u
#define R_GOLD  0xffd54au
#define R_CYAN  0x5bbcffu
#define R_OK    0x7fd88fu
#define R_WARN  0xff9a6cu

static int g_ansi;
static void rrect(int x,int y,int w,int h,uint32_t c){ if(g_ansi) lj_ansi_rect(x,y,w,h,c); else lj_render_rect(x,y,w,h,c); }
static void rtext(int x,int y,const char *s,uint32_t c,int maxw){ if(!s||!*s) return; if(g_ansi) lj_ansi_text(x,y,s,c,maxw); else lj_render_text(x,y,s,c,maxw); }

/* ⚠ Section gaps must be a WHOLE line. A half-line (LH/2) survives in the
 * window, where y is real pixels, but the terminal floors y to a cell — so the
 * same gap is visible in one backend and invisible in the other, and two
 * half-gaps silently add up to a full row. Spacing has to be identical in both
 * or "pixel perfect" means nothing. */
static int gap(int cy,int LH){ return cy + LH; }

static const char *jstr(json_object *o,const char *k){
    json_object *v=NULL;
    return (o&&json_object_object_get_ex(o,k,&v)&&json_object_is_type(v,json_type_string))?json_object_get_string(v):"";
}
static json_object *jget(json_object *o,const char *k){ json_object *v=NULL; if(o) json_object_object_get_ex(o,k,&v); return v; }
static int jint(json_object *o,const char *k){
    json_object *v=jget(o,k);
    return (v&&json_object_is_type(v,json_type_int))?json_object_get_int(v):0;
}
/* A state colours by what it DEMANDS, not by rank: blocked and active are the
 * ones a reader must act on, so they get the loud colours. */
static uint32_t state_colour(const char *st){
    if(!strcmp(st,"done"))    return R_OK;
    if(!strcmp(st,"active"))  return R_GOLD;
    if(!strcmp(st,"blocked")) return R_WARN;
    return R_DIM;
}

int lj_review_draw(json_object *snapshot,int x,int y,int w,int h,int scroll,int ansi){
    g_ansi=ansi;
    const int LH=LJ_LINE_H, PAD=LJ_CELL_W;
    /* Fixed rules own their rows; scrolling text cannot erase or share them. */
    int content_top=y+LH,content_bottom=y+h-LH;
    int top=content_top-scroll, cy=top, inner=w-PAD*2, tx=x+PAD;
    if(w<LJ_CELL_W*20||h<LH*3) return 0;   /* too small to say anything true */
    rrect(x,y,w,h,R_BG);
    rrect(x,y,w,1,R_EDGE);
    rrect(x,y+h-1,w,1,R_EDGE);

    char line[512];
    json_object *sum=jget(snapshot,"summary");
    if(!sum){
        /* ⚠ even the failure paths must respect the box: with scroll>0 an
         * unguarded draw lands OUTSIDE the tile (codex findings299). */
        if(cy>=content_top&&cy+LH<=content_bottom) rtext(tx,cy,"review data unavailable",R_WARN,inner);
        return 3*LH;
    }
    const char *serr=jstr(sum,"error");
    if(serr&&*serr){                        /* say WHY, never draw a false zero */
        snprintf(line,sizeof line,"board unreadable: %s",serr);
        if(cy>=content_top&&cy+LH<=content_bottom) rtext(tx,cy,line,R_WARN,inner);
        return 3*LH;
    }

    /* ── attention: what still needs the lead, and what is merely old ─────
     * This sits FIRST on purpose. A queue that replays an hour of handled
     * notices costs a turn each and buries the one message that still matters,
     * so stale items are counted rather than listed. */
    json_object *att=jget(snapshot,"attention");
    if(att){
        json_object *fresh=jget(att,"fresh");
        size_t fn=fresh?json_object_array_length(fresh):0;
        int stale=jint(att,"stale");
        if(cy>=content_top&&cy+LH<=content_bottom){
            if(fn) snprintf(line,sizeof line,"ATTENTION  %zu need you", fn);
            else   snprintf(line,sizeof line,"ATTENTION  nothing waiting");
            rtext(tx,cy,line,fn?R_WARN:R_OK,inner);
        }
        cy+=LH;
        for(size_t i=0;i<fn;i++){
            json_object *f=json_object_array_get_idx(fresh,i);
            if(cy>=content_top&&cy+LH<=content_bottom){
                snprintf(line,sizeof line,"  %3dm  %-10.10s %s",
                         jint(f,"age_min"),jstr(f,"sender"),jstr(f,"text"));
                rtext(tx,cy,line,R_TEXT,inner);
            }
            cy+=LH;
        }
        /* ⚠ NOT "already handled": these rows are state='pending', i.e. never
         * delivered, so age cannot prove anyone saw them. An old one may be old
         * BECAUSE it was never delivered. Say what the data supports. */
        if(stale>0&&cy>=content_top&&cy+LH<=content_bottom){
            snprintf(line,sizeof line,"  %d older notice%s still UNDELIVERED (oldest %dm) - not listed",
                     stale,stale==1?"":"s",jint(att,"oldest_stale_min"));
            rtext(tx,cy,line,R_DIM,inner);
        }
        if(stale>0) cy+=LH;
        cy=gap(cy,LH);
    }

    /* ── completion ─────────────────────────────────────────────────────── */
    int total=jint(sum,"total"),done=jint(sum,"done"),pct=jint(sum,"pct");
    snprintf(line,sizeof line,"SESSION  %d/%d done  %d%%   active %d · queued %d · blocked %d",
             done,total,pct,jint(sum,"active"),jint(sum,"queued"),jint(sum,"blocked"));
    if(cy>=content_top&&cy+LH<=content_bottom) rtext(tx,cy,line,R_GOLD,inner);
    cy+=LH;
    if(cy>=content_top&&cy+LH<=content_bottom&&total>0){         /* one bar, filled by real ratio */
        int bw=inner,fw=bw*pct/100;
        rrect(tx,cy+LH/3,bw,LH/3,R_EDGE);
        if(fw>0) rrect(tx,cy+LH/3,fw,LH/3,R_OK);
    }
    cy+=LH;

    json_object *per=jget(sum,"per_agent");
    if(per){
        json_object_object_foreach(per,agent,v){
            if(cy>=content_top&&cy+LH<=content_bottom){
                snprintf(line,sizeof line,"  %-9s %d/%d  %d%%",agent,jint(v,"done"),jint(v,"total"),jint(v,"pct"));
                rtext(tx,cy,line,R_CYAN,inner);
            }
            cy+=LH;
        }
    }
    cy=gap(cy,LH);

    /* ── todos: blocked and active lead, because they are what needs doing ── */
    json_object *todos=jget(snapshot,"todos");
    size_t n=todos?json_object_array_length(todos):0;
    /* ⚠ Columns sized from the TILE, not fixed. A hardcoded %-34s pushed state
     * and progress past the right edge of a narrow tile (codex findings299), so
     * the least important column is the one that shrinks. */
    int chars=inner/LJ_CELL_W, taskw=chars-30;
    if(taskw<10) taskw=10;
    if(taskw>34) taskw=34;
    if(cy>=content_top&&cy+LH<=content_bottom) rtext(tx,cy,"TODO",R_GOLD,inner);
    cy+=LH;
    for(size_t i=0;i<n;i++){
        json_object *t=json_object_array_get_idx(todos,i);
        const char *st=jstr(t,"state"),*pg=jstr(t,"progress");
        if(cy>=content_top&&cy+LH<=content_bottom){
            snprintf(line,sizeof line,"  %s %-9s %-*.*s %s%s%s",
                     jstr(t,"mark"),jstr(t,"agent"),taskw,taskw,jstr(t,"task"),st,
                     (pg&&*pg)?" · ":"",(pg&&*pg)?pg:"");
            rtext(tx,cy,line,state_colour(st),inner);
        }
        cy+=LH;
    }
    cy=gap(cy,LH);

    /* ── git ────────────────────────────────────────────────────────────── */
    json_object *git=jget(snapshot,"git");
    const char *gerr=jstr(git,"error");
    if(cy>=content_top&&cy+LH<=content_bottom) rtext(tx,cy,"GIT",R_GOLD,inner);
    cy+=LH;
    if(gerr&&*gerr){
        if(cy>=content_top&&cy+LH<=content_bottom){ snprintf(line,sizeof line,"  %s",gerr); rtext(tx,cy,line,R_WARN,inner); }
        cy+=LH;
    }else{
        json_object *rows=jget(git,"rows");
        size_t gn=rows?json_object_array_length(rows):0;
        for(size_t i=0;i<gn;i++){
            json_object *r=json_object_array_get_idx(rows,i);
            const char *sha=jstr(r,"sha");
            if(cy>=content_top&&cy+LH<=content_bottom){
                if(*sha) snprintf(line,sizeof line,"  %s%s %s  %s",jstr(r,"graph"),sha,jstr(r,"date"),jstr(r,"subject"));
                else     snprintf(line,sizeof line,"  %s",jstr(r,"graph"));
                rtext(tx,cy,line,*sha?R_TEXT:R_DIM,inner);
            }
            cy+=LH;
        }
    }
    cy=gap(cy,LH);

    /* ── fit ────────────────────────────────────────────────────────────── */
    json_object *fit=jget(snapshot,"fit");
    const char *ferr=jstr(fit,"error");
    if(cy>=content_top&&cy+LH<=content_bottom) rtext(tx,cy,"AGENT FIT",R_GOLD,inner);
    cy+=LH;
    if(ferr&&*ferr){
        /* An absent rating is reported. Treating "no score" as a score is how a
         * broken routing policy gets built out of silence. */
        if(cy>=content_top&&cy+LH<=content_bottom){ snprintf(line,sizeof line,"  not usable for routing: %s",ferr); rtext(tx,cy,line,R_DIM,inner); }
        cy+=LH;
    }else{
        json_object *sc=jget(fit,"scores");
        /* braces are required: the foreach macro opens with declarations, so it
         * cannot be the bare body of an if. */
        if(sc){ json_object_object_foreach(sc,who,v){
            if(cy>=content_top&&cy+LH<=content_bottom){
                json_object *sc_v=jget(v,"score");
                int have=sc_v&&!json_object_is_type(sc_v,json_type_null);
                /* ⚠ A null score is UNKNOWN, not 0.00. Printing 0.00 is exactly
                 * the "zero where it means unknown" this panel promises never to
                 * do — codex findings299 caught it in my own code. */
                if(have) snprintf(line,sizeof line,"  %-9s score %.2f  sessions %d",
                                  who,json_object_get_double(sc_v),jint(v,"sessions"));
                else     snprintf(line,sizeof line,"  %-9s no score recorded  sessions %d",
                                  who,jint(v,"sessions"));
                rtext(tx,cy,line,have?R_CYAN:R_DIM,inner);
            }
            cy+=LH;
        } }
    }

    /* ── room log: what was SAID, which is not the same question as what is
     * still owed. ATTENTION empties as notices are read; the thread must not. */
    json_object *log=jget(snapshot,"room_log");
    if(log){
        cy=gap(cy,LH);
        const char *lerr=jstr(log,"error");
        if(cy>=content_top&&cy+LH<=content_bottom) rtext(tx,cy,"ROOM LOG",R_GOLD,inner);
        cy+=LH;
        if(lerr&&*lerr){
            if(cy>=content_top&&cy+LH<=content_bottom){ snprintf(line,sizeof line,"  %s",lerr); rtext(tx,cy,line,R_WARN,inner); }
            cy+=LH;
        }else{
            json_object *lr=jget(log,"rows");
            size_t ln=lr?json_object_array_length(lr):0;
            for(size_t i=0;i<ln;i++){
                json_object *m=json_object_array_get_idx(lr,i);
                if(cy>=content_top&&cy+LH<=content_bottom){
                    snprintf(line,sizeof line,"  %s  %-12.12s %s",
                             jstr(m,"at"),jstr(m,"sender"),jstr(m,"text"));
                    rtext(tx,cy,line,R_TEXT,inner);
                }
                cy+=LH;
            }
        }
    }

    return cy-top+2*LH;
}
