/* Images on the room canvas (the operator: paste / drop / screenshot into the canvas;
 * agents: see what is on it). Pins: an image row is parsed and drawn into its
 * rect, the source is copied into rooms/<room>/media, a default rect keeps the
 * image's aspect on the 16:9 picture, later lines draw over the image, a path
 * with a quote survives the JSON, decoding is cached across polls, and
 * lj_canvas_save_png writes a PNG an agent can open. */
#define _GNU_SOURCE
#include "../liljack_app/c_canvas.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
/* Anti-aliased strokes: a pixel on the line is the stroke colour blended by
 * its coverage (>= 50% here), so compare channels with a tolerance. */
static int ink(uint32_t p,uint32_t c){for(int s=0;s<24;s+=8){int a=(int)((p>>s)&255),b=(int)((c>>s)&255);if(a-b>0x60||b-a>0x60)return 0;}return 1;}
static const char *ROOM="r-00000000000000000000000000000000";
/* 4x2 solid-red PNG, bytes generated once with zlib so the test needs no encoder. */
static const unsigned char RED_4x2[]={0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x02,0x08,0x02,0x00,0x00,0x00,0xf0,0xca,0xea,0x34,0x00,0x00,0x00,0x10,0x49,0x44,0x41,0x54,0x78,0x9c,0x63,0xf8,0xcf,0xc0,0x00,0x47,0x0c,0xc8,0x1c,0x00,0x6f,0xaa,0x07,0xf9,0x80,0xdc,0x00,0x28,0x00,0x00,0x00,0x00,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82};
static long fsize(const char *p){struct stat st;return stat(p,&st)?-1:(long)st.st_size;}
int main(void){
    char root[]="/tmp/liljack-canvas-image-XXXXXX";CHECK(mkdtemp(root));
    char src[600];snprintf(src,sizeof src,"%s/shot \"one\".png",root);   /* space + quote: JSON must escape */
    FILE *f=fopen(src,"wb");CHECK(f);CHECK(fwrite(RED_4x2,1,sizeof RED_4x2,f)==sizeof RED_4x2);fclose(f);
    lj_canvas c;lj_canvas_init(&c);CHECK(lj_canvas_open(&c,root,ROOM));
    const char *media=lj_canvas_media_dir(&c);CHECK(media&&strstr(media,"/rooms/r-0000")&&strstr(media,"/media"));
    /* explicit rect: left half of the picture */
    CHECK(lj_canvas_append_image(&c,src,0.0f,0.0f,0.5f,1.0f,"operator"));
    CHECK(lj_canvas_poll(&c)==1);CHECK(c.nstrokes==1&&c.strokes[0].kind==3);
    CHECK(!strncmp(c.strokes[0].file,media,strlen(media)));                /* copied into media/ */
    CHECK(fsize(c.strokes[0].file)==(long)sizeof RED_4x2);
    CHECK(strstr(c.strokes[0].file,"shot \"one\".png"));                     /* quote survived */
    float pts[]={0.0f,0.5f,1.0f,0.5f};CHECK(lj_canvas_append_line(&c,pts,2,0x00ff00,20,"claude"));
    CHECK(lj_canvas_poll(&c)==1&&c.nstrokes==2);
    const uint32_t *px=lj_canvas_render(&c,160,90,0x101010);CHECK(px);
    CHECK(px[10*160+20]==0xffff0000u);                                      /* inside the image */
    CHECK(px[10*160+120]==0xff101010u);                                     /* right half: background */
    CHECK(ink(px[45*160+20],0x00ff00));                                      /* later line draws over it */
    CHECK(c.images[0].px&&c.images[0].w==4&&c.images[0].h==2);              /* decoded once, cached */
    uint32_t *cached=c.images[0].px;
    CHECK(lj_canvas_append_line(&c,pts,2,0x0000ff,2,"codex"));CHECK(lj_canvas_poll(&c)==1);
    lj_canvas_render(&c,160,90,0x101010);CHECK(c.images[0].px==cached);    /* re-poll did not re-decode */
    /* default rect (x1<=x0): 4:2 image on a 16:9 picture keeps its aspect, centred */
    CHECK(lj_canvas_append_image(&c,c.strokes[0].file,0,0,0,0,"codex"));   /* already in media: no second copy */
    CHECK(lj_canvas_poll(&c)==1);lj_stroke *s=&c.strokes[c.nstrokes-1];CHECK(s->kind==3);
    float w=(s->points[2]-s->points[0])*16.0f,h=(s->points[3]-s->points[1])*9.0f;
    CHECK(w>0&&h>0&&w/h>1.95f&&w/h<2.05f);
    CHECK(s->points[0]+s->points[2]>0.99f&&s->points[0]+s->points[2]<1.01f);
    CHECK(!strcmp(s->file,c.strokes[0].file));
    /* a file that is not an image is refused with a reason, and nothing is appended */
    char bad[600];snprintf(bad,sizeof bad,"%s/notes.txt",root);f=fopen(bad,"w");fputs("hello",f);fclose(f);
    int before=c.nstrokes;CHECK(!lj_canvas_append_image(&c,bad,0,0,1,1,"operator"));CHECK(c.status[0]);
    lj_canvas_poll(&c);CHECK(c.nstrokes==before);
    /* agents' eyes */
    char out[600];snprintf(out,sizeof out,"%s/canvas.png",root);
    CHECK(lj_canvas_save_png(&c,out,320,180,0x101010));CHECK(fsize(out)>60);
    f=fopen(out,"rb");unsigned char sig[8];CHECK(fread(sig,1,8,f)==8);fclose(f);CHECK(!memcmp(sig,RED_4x2,8));
    lj_canvas_close(&c);CHECK(c.images[0].px==NULL);
    char cmd[700];snprintf(cmd,sizeof cmd,"rm -rf -- %s",root);CHECK(system(cmd)==0);
    puts("canvas-image: PASS");return 0;
}
