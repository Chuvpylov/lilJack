#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "c_canvas.h"
#include <json-c/json.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/random.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
/* Private stb copies: the canvas links on its own (tests, `room --canvas-png`)
 * without c_media.o or the HUI headless backend. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"   /* static stb: we use a few entry points */
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_MAX_DIMENSIONS 8192
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "deps/stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "deps/stb_image_write.h"
#pragma GCC diagnostic pop

void lj_canvas_init(lj_canvas *c){memset(c,0,sizeof *c);}

uint32_t lj_canvas_author_colour(const char *by){
    if(!strcmp(by,"operator"))return 0xffd166;
    if(!strcmp(by,"claude"))return 0x33ccff;
    if(!strcmp(by,"codex"))return 0x7ee787;
    if(!strcmp(by,"deepseek"))return 0xff7b72;
    return 0xdddddd;
}

unsigned lj_canvas_author_bit(const char *by){
    if(!by)return 16;
    if(!strcmp(by,"operator"))return 1;
    if(!strcmp(by,"claude"))return 2;
    if(!strcmp(by,"codex"))return 4;
    if(!strcmp(by,"deepseek"))return 8;
    return 16;
}

int lj_canvas_open(lj_canvas *c,const char *root,const char *room_id){
    lj_canvas_close(c);
    if(!root||!*root){snprintf(c->status,sizeof c->status,"Canvas has no workspace root");return 0;}
    if(!room_id||strncmp(room_id,"r-",2)){snprintf(c->status,sizeof c->status,"Canvas needs a room · open one first");return 0;}
    char dir[512];snprintf(dir,sizeof dir,"%s/rooms/%s",root,room_id);
    char parent[512];snprintf(parent,sizeof parent,"%s/rooms",root);
    mkdir(parent,0700);mkdir(dir,0700);
    snprintf(c->path,sizeof c->path,"%s/canvas.jsonl",dir);
    snprintf(c->media,sizeof c->media,"%s/media",dir);
    snprintf(c->status,sizeof c->status,"Canvas · %s",room_id);
    c->mtime=0;c->size=-1;c->dirty=1;return 1;
}

static void free_strokes(lj_canvas *c){
    for(int i=0;i<c->nstrokes;i++)free(c->strokes[i].points);
    free(c->strokes);c->strokes=NULL;c->nstrokes=0;
}

static int parse_colour(const char *s,uint32_t *out){
    if(!s||s[0]!='#'||strlen(s)!=7)return 0;
    char *end;unsigned long v=strtoul(s+1,&end,16);if(*end)return 0;*out=(uint32_t)v;return 1;
}

static void new_id(char out[24]){
    unsigned char r[6];if(getrandom(r,sizeof r,0)!=(ssize_t)sizeof r){unsigned long v=(unsigned long)time(NULL)^(unsigned long)getpid()^(unsigned long)clock();memcpy(r,&v,sizeof r);}
    snprintf(out,24,"%02x%02x%02x%02x%02x%02x",r[0],r[1],r[2],r[3],r[4],r[5]);
}
static int id_ok(const char *s){
    size_t n=strlen(s);if(!n||n>=24)return 0;
    for(size_t i=0;i<n;i++){char ch=s[i];if(!((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')||ch=='-'||ch=='_'))return 0;}
    return 1;
}
static int find_id(lj_canvas *c,const char *id){for(int i=c->nstrokes-1;i>=0;i--)if(!strcmp(c->strokes[i].id,id))return i;return -1;}
/* Apply a del/move/style row to the strokes parsed so far (file order). */
static void apply_op(lj_canvas *c,json_object *o,const char *t){
    json_object *ids=NULL,*v=NULL;if(!json_object_object_get_ex(o,"ids",&ids)||!json_object_is_type(ids,json_type_array))return;
    float dx=0,dy=0;uint32_t colour=0;int has_colour=0;float width=0;
    if(!strcmp(t,"move")){if(!json_object_object_get_ex(o,"d",&v)||!json_object_is_type(v,json_type_array)||json_object_array_length(v)!=2)return;
        dx=(float)json_object_get_double(json_object_array_get_idx(v,0));dy=(float)json_object_get_double(json_object_array_get_idx(v,1));}
    if(!strcmp(t,"style")){
        if(json_object_object_get_ex(o,"c",&v)&&json_object_is_type(v,json_type_string))has_colour=parse_colour(json_object_get_string(v),&colour);
        if(json_object_object_get_ex(o,"w",&v)&&(json_object_is_type(v,json_type_int)||json_object_is_type(v,json_type_double)))width=(float)json_object_get_double(v);}
    for(size_t k=0;k<json_object_array_length(ids);k++){
        json_object *e=json_object_array_get_idx(ids,k);if(!e||!json_object_is_type(e,json_type_string))continue;
        int i=find_id(c,json_object_get_string(e));if(i<0)continue;lj_stroke *s=&c->strokes[i];
        if(!strcmp(t,"del")){free(s->points);memmove(s,s+1,(size_t)(c->nstrokes-i-1)*sizeof *s);c->nstrokes--;}
        else if(!strcmp(t,"move")){for(int n=0;n<s->npoints;n++){float x=s->points[n*2]+dx,y=s->points[n*2+1]+dy;
                s->points[n*2]=x<0?0:x>1?1:x;s->points[n*2+1]=y<0?0:y>1?1:y;}}
        else {if(has_colour)s->colour=colour;if(width>=1&&width<=100)s->width=width;}
    }
}
static int push(lj_canvas *c,lj_stroke *s){
    if(c->nstrokes>=LJ_CANVAS_MAX_STROKES){free(s->points);return 0;}
    lj_stroke *grown=realloc(c->strokes,(size_t)(c->nstrokes+1)*sizeof *grown);
    if(!grown){free(s->points);return 0;}
    c->strokes=grown;c->strokes[c->nstrokes++]=*s;return 1;
}

static void parse_line(lj_canvas *c,const char *line,int lineno){
    json_object *o=json_tokener_parse(line);if(!o||!json_object_is_type(o,json_type_object)){if(o)json_object_put(o);return;}
    json_object *v=NULL;const char *t=json_object_object_get_ex(o,"t",&v)&&json_object_is_type(v,json_type_string)?json_object_get_string(v):"";
    lj_stroke s;memset(&s,0,sizeof s);
    if(json_object_object_get_ex(o,"by",&v)&&json_object_is_type(v,json_type_string))snprintf(s.by,sizeof s.by,"%s",json_object_get_string(v));
    if(!strcmp(t,"clear")){free_strokes(c);json_object_put(o);return;}
    if(!strcmp(t,"del")||!strcmp(t,"move")||!strcmp(t,"style")){apply_op(c,o,t);json_object_put(o);return;}
    if(json_object_object_get_ex(o,"id",&v)&&json_object_is_type(v,json_type_string)&&id_ok(json_object_get_string(v)))snprintf(s.id,sizeof s.id,"%s",json_object_get_string(v));
    else snprintf(s.id,sizeof s.id,"L%d",lineno);
    if(!strcmp(t,"line"))s.kind=0;else if(!strcmp(t,"text"))s.kind=1;else if(!strcmp(t,"image"))s.kind=3;else{json_object_put(o);return;}
    if(s.kind==3){
        if(!json_object_object_get_ex(o,"f",&v)||!json_object_is_type(v,json_type_string)||json_object_get_string_len(v)>=(int)sizeof s.file){json_object_put(o);return;}
        snprintf(s.file,sizeof s.file,"%s",json_object_get_string(v));
    }
    s.colour=lj_canvas_author_colour(s.by);
    if(json_object_object_get_ex(o,"c",&v)&&json_object_is_type(v,json_type_string))parse_colour(json_object_get_string(v),&s.colour);
    s.width=2;if(json_object_object_get_ex(o,"w",&v)&&(json_object_is_type(v,json_type_int)||json_object_is_type(v,json_type_double)))s.width=(float)json_object_get_double(v);
    if(s.width<1)s.width=1;
    if(s.width>100)s.width=100;
    if(json_object_object_get_ex(o,"s",&v)&&json_object_is_type(v,json_type_string))snprintf(s.text,sizeof s.text,"%s",json_object_get_string(v));
    json_object *p=NULL;
    if(!json_object_object_get_ex(o,"p",&p)||!json_object_is_type(p,json_type_array)){json_object_put(o);return;}
    int n=(int)json_object_array_length(p);if(n<1){json_object_put(o);return;}
    s.points=calloc((size_t)n*2,sizeof(float));if(!s.points){json_object_put(o);return;}
    for(int i=0;i<n;i++){json_object *pt=json_object_array_get_idx(p,(size_t)i);
        if(!pt||!json_object_is_type(pt,json_type_array)||json_object_array_length(pt)!=2){free(s.points);json_object_put(o);return;}
        double x=json_object_get_double(json_object_array_get_idx(pt,0)),y=json_object_get_double(json_object_array_get_idx(pt,1));
        if(x<0||x>1||y<0||y>1){free(s.points);json_object_put(o);return;}
        s.points[i*2]=(float)x;s.points[i*2+1]=(float)y;}
    if(s.kind==3&&(n!=2||s.points[2]<=s.points[0]||s.points[3]<=s.points[1])){free(s.points);json_object_put(o);return;}
    s.npoints=n;push(c,&s);json_object_put(o);
}

int lj_canvas_poll(lj_canvas *c){
    if(!c->path[0])return 0;
    struct stat st;
    if(stat(c->path,&st)!=0){if(c->loaded&&c->nstrokes){free_strokes(c);c->dirty=1;c->loaded=0;return 1;}c->loaded=0;c->size=-1;return 0;}
    if(c->loaded&&st.st_mtime==c->mtime&&(long)st.st_size==c->size)return 0;
    FILE *f=fopen(c->path,"r");if(!f)return 0;
    free_strokes(c);
    char *line=NULL;size_t cap=0;ssize_t len;
    int lineno=0;
    while((len=getline(&line,&cap,f))>0){
        if(line[len-1]!='\n')break;               /* trailing partial line: writer still appending */
        lineno++;line[len-1]=0;if(line[0])parse_line(c,line,lineno);
    }
    free(line);fclose(f);
    c->mtime=st.st_mtime;c->size=(long)st.st_size;c->loaded=1;c->dirty=1;return 1;
}

static void add_damage(lj_canvas *c,float x0,float y0,float x1,float y1);
/* ── Anti-aliased strokes ────────────────────────────────────────────────
 * A stroke is rasterized into a coverage mask over its bounding box (max over
 * its segments, so joints do not double-blend) and composited once. Coverage
 * is the distance from the pixel centre to the pen's centre line: r+0.5-d. */
static unsigned char *mask_buf;static size_t mask_cap;
static void stroke_aa(uint32_t *px,int w,int h,const float *p,int n,float r,uint32_t colour){
    if(n<1)return;
    float fx0=p[0],fy0=p[1],fx1=p[0],fy1=p[1];
    for(int i=1;i<n;i++){if(p[i*2]<fx0)fx0=p[i*2];if(p[i*2]>fx1)fx1=p[i*2];if(p[i*2+1]<fy0)fy0=p[i*2+1];if(p[i*2+1]>fy1)fy1=p[i*2+1];}
    int bx0=(int)floorf(fx0-r-1),by0=(int)floorf(fy0-r-1),bx1=(int)ceilf(fx1+r+1),by1=(int)ceilf(fy1+r+1);
    if(bx0<0)bx0=0;if(by0<0)by0=0;if(bx1>w)bx1=w;if(by1>h)by1=h;
    int bw=bx1-bx0,bh=by1-by0;if(bw<1||bh<1)return;
    size_t need=(size_t)bw*bh;
    if(need>mask_cap){unsigned char *m=realloc(mask_buf,need);if(!m)return;mask_buf=m;mask_cap=need;}
    memset(mask_buf,0,need);
    for(int k=0;k<(n>1?n-1:1);k++){
        float ax=p[k*2],ay=p[k*2+1],cx=n>1?p[k*2+2]:ax,cy=n>1?p[k*2+3]:ay;
        int sx0=(int)floorf((ax<cx?ax:cx)-r-1),sx1=(int)ceilf((ax>cx?ax:cx)+r+1),sy0=(int)floorf((ay<cy?ay:cy)-r-1),sy1=(int)ceilf((ay>cy?ay:cy)+r+1);
        if(sx0<bx0)sx0=bx0;if(sy0<by0)sy0=by0;if(sx1>bx1)sx1=bx1;if(sy1>by1)sy1=by1;
        float dx=cx-ax,dy=cy-ay,l=dx*dx+dy*dy;
        for(int y=sy0;y<sy1;y++)for(int x=sx0;x<sx1;x++){
            float qx=x+0.5f-ax,qy=y+0.5f-ay,t=l>0?(qx*dx+qy*dy)/l:0;if(t<0)t=0;if(t>1)t=1;
            float ex=qx-t*dx,ey=qy-t*dy,d=sqrtf(ex*ex+ey*ey),cov=r+0.5f-d;
            if(cov<=0)continue;unsigned char v=cov>=1?255:(unsigned char)(cov*255.0f+0.5f);
            unsigned char *m=&mask_buf[(size_t)(y-by0)*bw+(x-bx0)];if(v>*m)*m=v;}
    }
    uint32_t cr=(colour>>16)&255,cg=(colour>>8)&255,cb=colour&255;
    for(int y=by0;y<by1;y++)for(int x=bx0;x<bx1;x++){unsigned a=mask_buf[(size_t)(y-by0)*bw+(x-bx0)];if(!a)continue;
        uint32_t *d=&px[(size_t)y*w+x];
        if(a==255){*d=0xff000000u|colour;continue;}
        uint32_t dr=(*d>>16)&255,dg=(*d>>8)&255,db=*d&255;
        *d=0xff000000u|((cr*a+dr*(255-a))/255)<<16|((cg*a+dg*(255-a))/255)<<8|((cb*a+db*(255-a))/255);}
}

/* ── One curve for preview AND committed stroke ───────────────────────────
 * Midpoint quadratic smoothing: the path runs P0 -> M01, then a quadratic
 * through each inner point Pi from M(i-1,i) to M(i,i+1), then M(n-2,n-1) -> Pn-1.
 * Everything up to the last midpoint is FINAL once point i+1 exists, so the live
 * preview draws only new, final pieces and the released stroke is the same
 * curve plus its short tail: nothing jumps on release, and cell-quantised or
 * jittery input still reads as a smooth line. Output is in pixel coordinates;
 * pieces from `done` (points already drawn) onward; `tail` adds the last piece. */
static float *curve(const float *pts,int n,int done,int tail,int w,int h,int *outn){
    *outn=0;if(n<1)return NULL;
    size_t cap=(size_t)(n+2)*18*2;float *o=malloc(cap*sizeof(float));if(!o)return NULL;int k=0;
#define PT(x,y) do{o[k*2]=(x)*w;o[k*2+1]=(y)*h;k++;}while(0)
    if(n==1){if(done<1)PT(pts[0],pts[1]);*outn=k;return o;}
    float mx,my;
    if(done<2){PT(pts[0],pts[1]);mx=(pts[0]+pts[2])/2;my=(pts[1]+pts[3])/2;PT(mx,my);}
    for(int i=done<2?1:done-1;i<=n-2;i++){
        float ax=(pts[(i-1)*2]+pts[i*2])/2,ay=(pts[(i-1)*2+1]+pts[i*2+1])/2;
        float bx=(pts[i*2]+pts[(i+1)*2])/2,by=(pts[i*2+1]+pts[(i+1)*2+1])/2;
        float cx=pts[i*2],cy=pts[i*2+1];
        float len=(fabsf(bx-ax)*w+fabsf(by-ay)*h)+(fabsf(cx-ax)*w+fabsf(cy-ay)*h);
        int steps=(int)(len/3.0f);if(steps<2)steps=2;if(steps>16)steps=16;
        if(k==0)PT(ax,ay);
        for(int s=1;s<=steps;s++){float t=(float)s/steps,u=1-t;PT(u*u*ax+2*u*t*cx+t*t*bx,u*u*ay+2*u*t*cy+t*t*by);}
    }
    if(tail){if(k==0){PT((pts[(n-2)*2]+pts[(n-1)*2])/2,(pts[(n-2)*2+1]+pts[(n-1)*2+1])/2);}PT(pts[(n-1)*2],pts[(n-1)*2+1]);}
#undef PT
    *outn=k;return o;
}
static void add_damage(lj_canvas *c,float x0,float y0,float x1,float y1);
/* Decode once per path; least-recently-used slot is recycled. A file that
 * fails to decode is cached as a miss (px NULL, w -1) so it is not retried per frame. */
static lj_canvas_image *image_get(lj_canvas *c,const char *path){
    lj_canvas_image *slot=NULL;
    for(int i=0;i<LJ_CANVAS_IMAGE_CACHE;i++){lj_canvas_image *e=&c->images[i];
        if(e->path[0]&&!strcmp(e->path,path)){e->used=++c->image_clock;return e->px?e:NULL;}
        if(!e->path[0]){if(!slot||slot->path[0])slot=e;}      /* first empty slot wins */
        else if(!slot||(slot->path[0]&&e->used<slot->used))slot=e;}
    free(slot->px);memset(slot,0,sizeof *slot);snprintf(slot->path,sizeof slot->path,"%s",path);slot->used=++c->image_clock;
    int w=0,h=0,n=0;unsigned char *rgba=stbi_load(path,&w,&h,&n,4);
    if(!rgba){slot->w=-1;return NULL;}
    slot->px=malloc((size_t)w*h*sizeof(uint32_t));
    if(slot->px){for(size_t i=0;i<(size_t)w*h;i++){const unsigned char *q=rgba+i*4;slot->px[i]=(uint32_t)q[3]<<24|(uint32_t)q[0]<<16|(uint32_t)q[1]<<8|q[2];}slot->w=w;slot->h=h;}
    stbi_image_free(rgba);return slot->px?slot:NULL;
}
/* Scale the image into its rect: box-average when shrinking (screenshots stay
 * legible), nearest when enlarging; alpha blends over what is already there. */
static void draw_image(uint32_t *px,int w,int h,const lj_canvas_image *im,const float *p){
    int rx0=(int)floorf(p[0]*w),ry0=(int)floorf(p[1]*h),rx1=(int)ceilf(p[2]*w),ry1=(int)ceilf(p[3]*h);
    int rw=rx1-rx0,rh=ry1-ry0;if(rw<1||rh<1)return;
    for(int y=ry0<0?0:ry0;y<ry1&&y<h;y++){
        int sy0=(int)((long)(y-ry0)*im->h/rh),sy1=(int)((long)(y-ry0+1)*im->h/rh);if(sy1<=sy0)sy1=sy0+1;if(sy1>im->h)sy1=im->h;
        for(int x=rx0<0?0:rx0;x<rx1&&x<w;x++){
            int sx0=(int)((long)(x-rx0)*im->w/rw),sx1=(int)((long)(x-rx0+1)*im->w/rw);if(sx1<=sx0)sx1=sx0+1;if(sx1>im->w)sx1=im->w;
            unsigned a=0,r=0,g=0,b=0,k=0;
            for(int sy=sy0;sy<sy1;sy++)for(int sx=sx0;sx<sx1;sx++){uint32_t v=im->px[(size_t)sy*im->w+sx];a+=v>>24;r+=v>>16&255;g+=v>>8&255;b+=v&255;k++;}
            a/=k;r/=k;g/=k;b/=k;uint32_t d=px[(size_t)y*w+x];
            if(a<255){r=(r*a+((d>>16)&255)*(255-a))/255;g=(g*a+((d>>8)&255)*(255-a))/255;b=(b*a+(d&255)*(255-a))/255;}
            px[(size_t)y*w+x]=0xff000000u|r<<16|g<<8|b;}}
}
const uint32_t *lj_canvas_render(lj_canvas *c,int w,int h,uint32_t background){
    if(w<1||h<1)return NULL;
    if(w!=c->w||h!=c->h){free(c->pixels);c->pixels=malloc((size_t)w*h*sizeof(uint32_t));if(!c->pixels){c->w=c->h=0;return NULL;}c->w=w;c->h=h;c->dirty=1;}
    else if(!c->dirty)return c->pixels;
    for(size_t i=0;i<(size_t)w*h;i++)c->pixels[i]=0xff000000u|background;
    float scale=(float)(w<h?w:h)/1000.0f;
    for(int i=0;i<c->nstrokes;i++){lj_stroke *s=&c->strokes[i];float r=s->width*scale*0.5f;if(r<0.75f)r=0.75f;
        if(c->hidden&lj_canvas_author_bit(s->by))continue;
        if(s->kind==3){lj_canvas_image *im=image_get(c,s->file);
            if(im)draw_image(c->pixels,w,h,im,s->points);
            else {float q[]={s->points[0]*w,s->points[1]*h,s->points[2]*w,s->points[1]*h,s->points[2]*w,s->points[3]*h,s->points[0]*w,s->points[3]*h,s->points[0]*w,s->points[1]*h};
                stroke_aa(c->pixels,w,h,q,5,1,0x884444);}          /* missing file: an outline says where it was */
            continue;}
        if(s->kind==1){float q[]={s->points[0]*w,s->points[1]*h};stroke_aa(c->pixels,w,h,q,1,r*2+2,s->colour);
            /* text glyphs live in the host font; the marker says where the note is */
            continue;}
        int sn=0;float *sp=curve(s->points,s->npoints,0,1,w,h,&sn);
        if(sp){stroke_aa(c->pixels,w,h,sp,sn,r,s->colour);free(sp);}
    }
    c->dirty=0;c->preview_n=0;add_damage(c,0,0,1,1);return c->pixels;
}

int lj_canvas_append_line(lj_canvas *c,const float *points,int npoints,uint32_t colour,float width,const char *by){
    if(!c->path[0]||npoints<1||npoints>LJ_CANVAS_MAX_POINTS)return 0;
    FILE *f=fopen(c->path,"a+");if(!f)return 0;
    /* Another writer's unfinished line must not swallow this stroke: start fresh. */
    if(fseek(f,-1,SEEK_END)==0){int last=fgetc(f);if(last!=EOF&&last!='\n')fputc('\n',f);}
    fseek(f,0,SEEK_END);
    time_t now=time(NULL);struct tm tm;gmtime_r(&now,&tm);char at[32];strftime(at,sizeof at,"%Y-%m-%dT%H:%M:%SZ",&tm);
    fprintf(f,"{\"t\":\"line\",\"c\":\"#%06x\",\"w\":%g,\"p\":[",colour&0xffffff,width);
    for(int i=0;i<npoints;i++){float x=points[i*2],y=points[i*2+1];if(x<0)x=0;if(x>1)x=1;if(y<0)y=0;if(y>1)y=1;
        fprintf(f,"%s[%.4f,%.4f]",i?",":"",x,y);}
    char id[24];new_id(id);
    fprintf(f,"],\"by\":\"%s\",\"at\":\"%s\",\"id\":\"%s\"}\n",by&&*by?by:"operator",at,id);
    int ok=fflush(f)==0;fsync(fileno(f));fclose(f);return ok;
}

void lj_canvas_close(lj_canvas *c){for(int i=0;i<LJ_CANVAS_IMAGE_CACHE;i++)free(c->images[i].px);memset(c->images,0,sizeof c->images);c->image_clock=0;c->media[0]=0;free_strokes(c);free(c->pixels);c->pixels=NULL;c->w=c->h=0;c->path[0]=0;c->loaded=0;c->size=-1;c->mtime=0;}

static void add_damage(lj_canvas *c,float x0,float y0,float x1,float y1){
    if(x0<0)x0=0;if(y0<0)y0=0;if(x1>1)x1=1;if(y1>1)y1=1;if(x1<x0||y1<y0)return;
    if(!c->damaged){c->damage[0]=x0;c->damage[1]=y0;c->damage[2]=x1;c->damage[3]=y1;c->damaged=1;return;}
    if(x0<c->damage[0])c->damage[0]=x0;if(y0<c->damage[1])c->damage[1]=y0;
    if(x1>c->damage[2])c->damage[2]=x1;if(y1>c->damage[3])c->damage[3]=y1;
}
int lj_canvas_take_damage(lj_canvas *c,float out[4]){
    if(!c->damaged)return 0;
    for(int i=0;i<4;i++)out[i]=c->damage[i];
    c->damaged=0;return 1;
}
/* Preview of the stroke the operator is still dragging, painted over the last render.
 * Only the curve pieces that became FINAL since the previous call are drawn
 * (see curve()); the released stroke is the same curve plus its tail, so it
 * does not jump. The file stays the only truth. */
void lj_canvas_preview(lj_canvas *c,const float *points,int npoints,uint32_t colour,float width){
    if(!c->pixels||npoints<1)return;
    if(npoints<c->preview_n)c->preview_n=0;                  /* a new stroke */
    if(npoints==c->preview_n)return;
    float scale=(float)(c->w<c->h?c->w:c->h)/1000.0f,r=width*scale*0.5f;if(r<0.75f)r=0.75f;
    int n=0;float *q=curve(points,npoints,c->preview_n,0,c->w,c->h,&n);
    if(q&&n){stroke_aa(c->pixels,c->w,c->h,q,n,r,colour);
        float x0=q[0],y0=q[1],x1=q[0],y1=q[1];
        for(int i=1;i<n;i++){if(q[i*2]<x0)x0=q[i*2];if(q[i*2]>x1)x1=q[i*2];if(q[i*2+1]<y0)y0=q[i*2+1];if(q[i*2+1]>y1)y1=q[i*2+1];}
        add_damage(c,(x0-r-1)/c->w,(y0-r-1)/c->h,(x1+r+1)/c->w,(y1+r+1)/c->h);}
    free(q);c->preview_n=npoints;
}

const char *lj_canvas_media_dir(lj_canvas *c){
    if(!c->media[0])return "";
    mkdir(c->media,0700);return c->media;
}

/* Copy src into media/ as <UTC time>-<basename>, adding -N until the name is free.
 * A file already inside media/ is used as is. */
int lj_canvas_import(lj_canvas *c,const char *src,char *out,size_t outcap){
    const char *dir=lj_canvas_media_dir(c);
    if(!*dir){snprintf(c->status,sizeof c->status,"Canvas is not open");return 0;}
    size_t dl=strlen(dir);
    if(!strncmp(src,dir,dl)&&src[dl]=='/'&&!strchr(src+dl+1,'/')){if((size_t)snprintf(out,outcap,"%s",src)>=outcap)return 0;return 1;}
    int in=open(src,O_RDONLY|O_CLOEXEC);if(in<0){snprintf(c->status,sizeof c->status,"Cannot read %.80s: %s",src,strerror(errno));return 0;}
    struct stat st;if(fstat(in,&st)||!S_ISREG(st.st_mode)){close(in);snprintf(c->status,sizeof c->status,"Not a regular file: %.80s",src);return 0;}
    const char *base=strrchr(src,'/');base=base?base+1:src;
    time_t now=time(NULL);struct tm tm;gmtime_r(&now,&tm);char stamp[32];strftime(stamp,sizeof stamp,"%Y%m%dT%H%M%SZ",&tm);
    int fd=-1;
    for(int k=0;k<1000&&fd<0;k++){
        int n=k?snprintf(out,outcap,"%s/%s-%d-%s",dir,stamp,k,base):snprintf(out,outcap,"%s/%s-%s",dir,stamp,base);
        if(n<0||(size_t)n>=outcap){close(in);snprintf(c->status,sizeof c->status,"Media path too long");return 0;}
        fd=open(out,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
        if(fd<0&&errno!=EEXIST)break;}
    if(fd<0){close(in);snprintf(c->status,sizeof c->status,"Cannot create media copy: %s",strerror(errno));return 0;}
    char buf[65536];ssize_t r;int ok=1;
    while((r=read(in,buf,sizeof buf))>0){for(ssize_t o=0;o<r;){ssize_t w=write(fd,buf+o,(size_t)(r-o));if(w<=0){ok=0;break;}o+=w;}if(!ok)break;}
    if(r<0)ok=0;
    if(ok&&fsync(fd))ok=0;
    close(in);if(close(fd))ok=0;
    if(!ok){unlink(out);snprintf(c->status,sizeof c->status,"Copy into media failed");return 0;}
    return 1;
}

int lj_canvas_append_image(lj_canvas *c,const char *src,float x0,float y0,float x1,float y1,const char *by){
    if(!c->path[0]||!src||!*src){snprintf(c->status,sizeof c->status,"Canvas is not open");return 0;}
    int iw=0,ih=0,n=0;
    if(!stbi_info(src,&iw,&ih,&n)||iw<1||ih<1){snprintf(c->status,sizeof c->status,"Not an image (png jpg gif bmp tga): %.60s",src);return 0;}
    if(x1<=x0||y1<=y0){                         /* default: centred, aspect kept, <= 60% of the picture */
        float aspect=(float)iw/(float)ih/LJ_CANVAS_ASPECT,w=0.6f,h=w/aspect;   /* in normalized units */
        if(h>0.6f){h=0.6f;w=h*aspect;}
        x0=0.5f-w/2;x1=0.5f+w/2;y0=0.5f-h/2;y1=0.5f+h/2;}
    if(x0<0)x0=0;if(y0<0)y0=0;if(x1>1)x1=1;if(y1>1)y1=1;
    if(x1<=x0||y1<=y0){snprintf(c->status,sizeof c->status,"Image rect is empty");return 0;}
    char copy[1024];if(!lj_canvas_import(c,src,copy,sizeof copy))return 0;
    json_object *o=json_object_new_object();
    time_t now=time(NULL);struct tm tm;gmtime_r(&now,&tm);char at[32];strftime(at,sizeof at,"%Y-%m-%dT%H:%M:%SZ",&tm);
    json_object *p=json_object_new_array(),*a=json_object_new_array(),*b=json_object_new_array();
    json_object_array_add(a,json_object_new_double(x0));json_object_array_add(a,json_object_new_double(y0));
    json_object_array_add(b,json_object_new_double(x1));json_object_array_add(b,json_object_new_double(y1));
    json_object_array_add(p,a);json_object_array_add(p,b);
    json_object_object_add(o,"t",json_object_new_string("image"));
    json_object_object_add(o,"f",json_object_new_string(copy));
    json_object_object_add(o,"p",p);
    json_object_object_add(o,"by",json_object_new_string(by&&*by?by:"operator"));
    json_object_object_add(o,"at",json_object_new_string(at));
    char id[24];new_id(id);json_object_object_add(o,"id",json_object_new_string(id));
    const char *row=json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN|JSON_C_TO_STRING_NOSLASHESCAPE);
    FILE *f=fopen(c->path,"a+");int ok=0;
    if(f){if(fseek(f,-1,SEEK_END)==0){int last=fgetc(f);if(last!=EOF&&last!='\n')fputc('\n',f);}
        fseek(f,0,SEEK_END);ok=fprintf(f,"%s\n",row)>0&&fflush(f)==0;fsync(fileno(f));fclose(f);}
    json_object_put(o);
    if(!ok)snprintf(c->status,sizeof c->status,"Cannot append to the canvas file");
    return ok;
}

int lj_canvas_save_png(lj_canvas *c,const char *out,int w,int h,uint32_t background){
    lj_canvas_poll(c);c->dirty=1;
    const uint32_t *px=lj_canvas_render(c,w,h,background);if(!px)return 0;
    unsigned char *rgb=malloc((size_t)w*h*3);if(!rgb)return 0;
    for(size_t i=0;i<(size_t)w*h;i++){rgb[i*3]=px[i]>>16&255;rgb[i*3+1]=px[i]>>8&255;rgb[i*3+2]=px[i]&255;}
    int ok=stbi_write_png(out,w,h,3,rgb,w*3)!=0;free(rgb);
    if(!ok)snprintf(c->status,sizeof c->status,"Cannot write %.80s",out);
    return ok;
}

const char *lj_canvas_stroke_id(lj_canvas *c,int i){return i>=0&&i<c->nstrokes?c->strokes[i].id:NULL;}

int lj_canvas_bbox(lj_canvas *c,int i,float out[4]){
    if(i<0||i>=c->nstrokes||c->strokes[i].npoints<1)return 0;
    lj_stroke *s=&c->strokes[i];out[0]=out[1]=1;out[2]=out[3]=0;
    for(int n=0;n<s->npoints;n++){float x=s->points[n*2],y=s->points[n*2+1];
        if(x<out[0])out[0]=x;if(y<out[1])out[1]=y;if(x>out[2])out[2]=x;if(y>out[3])out[3]=y;}
    return 1;
}

/* Distances in picture-HEIGHT units: x is stretched by the 16:9 aspect so a
 * radius is round on screen. A pen width w (1000px reference over the height)
 * adds w/2000. */
static float seg_dist(float px,float py,float x0,float y0,float x1,float y1){
    px*=LJ_CANVAS_ASPECT;x0*=LJ_CANVAS_ASPECT;x1*=LJ_CANVAS_ASPECT;
    float dx=x1-x0,dy=y1-y0,l=dx*dx+dy*dy,t=l>0?((px-x0)*dx+(py-y0)*dy)/l:0;if(t<0)t=0;if(t>1)t=1;
    float ex=x0+t*dx-px,ey=y0+t*dy-py;return sqrtf(ex*ex+ey*ey);
}
int lj_canvas_hit(lj_canvas *c,float x,float y,float radius){
    for(int i=c->nstrokes-1;i>=0;i--){lj_stroke *s=&c->strokes[i];
        if(c->hidden&lj_canvas_author_bit(s->by))continue;
        if(s->kind==3){float r=radius/LJ_CANVAS_ASPECT;
            if(x>=s->points[0]-r&&x<=s->points[2]+r&&y>=s->points[1]-radius&&y<=s->points[3]+radius)return i;continue;}
        float reach=radius+(s->kind==1?0.02f:s->width/2000.0f);
        if(s->npoints==1||s->kind==1){if(seg_dist(x,y,s->points[0],s->points[1],s->points[0],s->points[1])<=reach)return i;continue;}
        for(int n=1;n<s->npoints;n++)if(seg_dist(x,y,s->points[(n-1)*2],s->points[(n-1)*2+1],s->points[n*2],s->points[n*2+1])<=reach)return i;
    }
    return -1;
}

static int append_row(lj_canvas *c,json_object *o){
    const char *row=json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN|JSON_C_TO_STRING_NOSLASHESCAPE);
    FILE *f=fopen(c->path,"a+");if(!f){snprintf(c->status,sizeof c->status,"Cannot append to the canvas file");return 0;}
    if(fseek(f,-1,SEEK_END)==0){int last=fgetc(f);if(last!=EOF&&last!='\n')fputc('\n',f);}
    fseek(f,0,SEEK_END);int ok=fprintf(f,"%s\n",row)>0&&fflush(f)==0;fsync(fileno(f));fclose(f);
    if(!ok)snprintf(c->status,sizeof c->status,"Cannot append to the canvas file");
    return ok;
}
static int refuse(lj_canvas *c,json_object *o,const char *why){snprintf(c->status,sizeof c->status,"%s",why);if(o)json_object_put(o);return 0;}
int lj_canvas_append_op(lj_canvas *c,const char *json_row,const char *by){
    if(!c->path[0])return refuse(c,NULL,"Canvas is not open");
    json_object *o=json_row?json_tokener_parse(json_row):NULL,*v=NULL,*ids=NULL;
    if(!o||!json_object_is_type(o,json_type_object))return refuse(c,o,"Edit op must be a JSON object");
    const char *t=json_object_object_get_ex(o,"t",&v)&&json_object_is_type(v,json_type_string)?json_object_get_string(v):"";
    if(strcmp(t,"del")&&strcmp(t,"move")&&strcmp(t,"style"))return refuse(c,o,"Edit op t is del, move or style");
    if(!json_object_object_get_ex(o,"ids",&ids)||!json_object_is_type(ids,json_type_array)||json_object_array_length(ids)<1||json_object_array_length(ids)>1024)
        return refuse(c,o,"Edit op needs ids: 1..1024 stroke ids");
    for(size_t k=0;k<json_object_array_length(ids);k++){json_object *e=json_object_array_get_idx(ids,k);
        if(!e||!json_object_is_type(e,json_type_string)||!id_ok(json_object_get_string(e)))return refuse(c,o,"Stroke ids are 1..23 of [A-Za-z0-9_-]");}
    if(!strcmp(t,"move")){
        if(!json_object_object_get_ex(o,"d",&v)||!json_object_is_type(v,json_type_array)||json_object_array_length(v)!=2)return refuse(c,o,"move needs d: [dx, dy]");
        for(int k=0;k<2;k++){json_object *e=json_object_array_get_idx(v,(size_t)k);double dv=e?json_object_get_double(e):9;
            if(!e||!(json_object_is_type(e,json_type_int)||json_object_is_type(e,json_type_double))||dv<-1||dv>1)return refuse(c,o,"move d is two numbers in -1..1");}}
    if(!strcmp(t,"style")){int any=0;uint32_t col;
        if(json_object_object_get_ex(o,"c",&v)){if(!json_object_is_type(v,json_type_string)||!parse_colour(json_object_get_string(v),&col))return refuse(c,o,"style c is #rrggbb");any=1;}
        if(json_object_object_get_ex(o,"w",&v)){double w=json_object_get_double(v);if(!(json_object_is_type(v,json_type_int)||json_object_is_type(v,json_type_double))||w<1||w>100)return refuse(c,o,"style w is 1..100");any=1;}
        if(!any)return refuse(c,o,"style needs c and/or w");}
    time_t now=time(NULL);struct tm tm;gmtime_r(&now,&tm);char at[32];strftime(at,sizeof at,"%Y-%m-%dT%H:%M:%SZ",&tm);
    json_object_object_add(o,"by",json_object_new_string(by&&*by?by:"operator"));
    json_object_object_add(o,"at",json_object_new_string(at));
    int ok=append_row(c,o);json_object_put(o);return ok;
}
int lj_canvas_undo(lj_canvas *c,const char *by){
    lj_canvas_poll(c);
    for(int i=c->nstrokes-1;i>=0;i--)if(!strcmp(c->strokes[i].by,by&&*by?by:"operator")){
        char row[96];snprintf(row,sizeof row,"{\"t\":\"del\",\"ids\":[\"%s\"]}",c->strokes[i].id);
        return lj_canvas_append_op(c,row,by);}
    snprintf(c->status,sizeof c->status,"Nothing of yours to undo");return 0;
}
