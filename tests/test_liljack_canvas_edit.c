/* Canvas editing (the operator: "proper controls: selection, removal, AI supported").
 * Pins: every appended row carries a stable id (old rows get L<line>), hit-test
 * picks the TOPMOST stroke under the pointer (lines by distance incl. pen width,
 * images by rect), bbox per stroke, op rows del/move/style apply in file order
 * and survive a re-poll, undo removes the author's own last stroke only, and a
 * malformed op is refused before any byte is written. */
#define _GNU_SOURCE
#include "../liljack_app/c_canvas.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
static const char *ROOM="r-00000000000000000000000000000000";
static long fsize(const char *p){struct stat st;return stat(p,&st)?-1:(long)st.st_size;}
int main(void){
    char root[]="/tmp/liljack-canvas-edit-XXXXXX";CHECK(mkdtemp(root));
    lj_canvas c;lj_canvas_init(&c);CHECK(lj_canvas_open(&c,root,ROOM));
    FILE *f=fopen(c.path,"w");CHECK(f);
    fputs("{\"t\":\"line\",\"p\":[[0.1,0.5],[0.9,0.5]],\"by\":\"codex\"}\n",f);      /* legacy row: no id */
    fclose(f);
    float v[]={0.5f,0.1f,0.5f,0.9f};CHECK(lj_canvas_append_line(&c,v,2,0xff0000,4,"operator"));
    float d[]={0.2f,0.2f,0.3f,0.3f};CHECK(lj_canvas_append_line(&c,d,2,0x00ff00,4,"claude"));
    CHECK(lj_canvas_poll(&c)==1&&c.nstrokes==3);
    CHECK(!strcmp(lj_canvas_stroke_id(&c,0),"L1"));                            /* legacy id = line number */
    const char *vid=lj_canvas_stroke_id(&c,1);CHECK(vid&&strlen(vid)>=6&&vid[0]!='L');
    CHECK(strcmp(lj_canvas_stroke_id(&c,1),lj_canvas_stroke_id(&c,2)));
    char vkeep[32];snprintf(vkeep,sizeof vkeep,"%s",vid);
    /* hit: the crossing point is covered by both lines; the later (operator, vertical) wins */
    CHECK(lj_canvas_hit(&c,0.5f,0.5f,0.01f)==1);
    CHECK(lj_canvas_hit(&c,0.2f,0.5f,0.01f)==0);                               /* only the horizontal */
    CHECK(lj_canvas_hit(&c,0.25f,0.25f,0.01f)==2);                             /* on the short diagonal */
    CHECK(lj_canvas_hit(&c,0.8f,0.9f,0.01f)==-1);                              /* empty spot */
    float b[4];CHECK(lj_canvas_bbox(&c,1,b)&&b[0]<=0.5f&&b[2]>=0.5f&&b[1]<=0.1f&&b[3]>=0.9f);
    /* delete by id */
    char op[256];snprintf(op,sizeof op,"{\"t\":\"del\",\"ids\":[\"%s\"]}",vkeep);
    CHECK(lj_canvas_append_op(&c,op,"operator"));CHECK(lj_canvas_poll(&c)==1&&c.nstrokes==2);
    CHECK(lj_canvas_hit(&c,0.5f,0.5f,0.01f)==0);                               /* now the horizontal */
    /* move the legacy line down by 0.2 */
    CHECK(lj_canvas_append_op(&c,"{\"t\":\"move\",\"ids\":[\"L1\"],\"d\":[0,0.2]}","claude"));
    CHECK(lj_canvas_poll(&c)==1);CHECK(lj_canvas_hit(&c,0.2f,0.7f,0.01f)==0&&lj_canvas_hit(&c,0.2f,0.5f,0.01f)==-1);
    /* restyle */
    CHECK(lj_canvas_append_op(&c,"{\"t\":\"style\",\"ids\":[\"L1\"],\"c\":\"#123456\",\"w\":9}","operator"));
    CHECK(lj_canvas_poll(&c)==1&&c.strokes[0].colour==0x123456&&c.strokes[0].width==9);
    /* refused: bad kind, no ids, bad colour, bad delta; file unchanged */
    long size=fsize(c.path);
    CHECK(!lj_canvas_append_op(&c,"{\"t\":\"nuke\",\"ids\":[\"L1\"]}","operator"));
    CHECK(!lj_canvas_append_op(&c,"{\"t\":\"del\",\"ids\":[]}","operator"));
    CHECK(!lj_canvas_append_op(&c,"{\"t\":\"style\",\"ids\":[\"L1\"],\"c\":\"red\"}","operator"));
    CHECK(!lj_canvas_append_op(&c,"{\"t\":\"move\",\"ids\":[\"L1\"],\"d\":[5,0]}","operator"));
    CHECK(!lj_canvas_append_op(&c,"not json","operator"));
    CHECK(fsize(c.path)==size&&c.status[0]);
    /* undo: operator's last own stroke; claude's stroke is untouched */
    float e[]={0.6f,0.6f,0.7f,0.7f};CHECK(lj_canvas_append_line(&c,e,2,0xffffff,4,"operator"));
    CHECK(lj_canvas_poll(&c)==1&&c.nstrokes==3);
    CHECK(lj_canvas_undo(&c,"operator"));CHECK(lj_canvas_poll(&c)==1&&c.nstrokes==2);
    CHECK(lj_canvas_hit(&c,0.65f,0.65f,0.01f)==-1&&lj_canvas_hit(&c,0.25f,0.25f,0.01f)>=0);
    CHECK(!lj_canvas_undo(&c,"deepseek"));                                      /* nothing of theirs */
    /* image hit is by rect */
    f=fopen(c.path,"a");fputs("{\"t\":\"image\",\"f\":\"/nonexistent.png\",\"p\":[[0.8,0.8],[0.95,0.95]],\"by\":\"codex\",\"id\":\"img1\"}\n",f);fclose(f);
    CHECK(lj_canvas_poll(&c)==1);CHECK(lj_canvas_hit(&c,0.9f,0.9f,0.0f)==c.nstrokes-1);
    CHECK(!strcmp(lj_canvas_stroke_id(&c,c.nstrokes-1),"img1"));
    lj_canvas_close(&c);
    char cmd[600];snprintf(cmd,sizeof cmd,"rm -rf -- %s",root);CHECK(system(cmd)==0);
    puts("canvas-edit: PASS");return 0;
}
