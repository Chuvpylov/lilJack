/* Include after main.c in demo capture fixtures. Never set cached hover state:
 * render derives it from the pointer and the previous frame's hit rectangles.
 * Use the returned rectangle for subsequent click/RMB events when required. */
#ifndef LILJACK_CAPTURE_POINTER_H
#define LILJACK_CAPTURE_POINTER_H
#include <assert.h>
static Hit capture_hover_control(App *a,int kind,int index,const char *id){
    a->mousex=-1;a->mousey=-1;
    render(a); /* Populate the current layout's hit rectangles. */
    Hit target={0};int found=0;
    for(int i=0;i<a->nhits;i++){
        Hit *h=&a->hits[i];
        if(h->kind==kind&&h->index==index&&!strcmp(h->id,id?id:"")){
            target=*h;found++;
        }
    }
    assert(found==1&&target.r.w>0&&target.r.h>0);
    a->mousex=target.r.x+target.r.w/2;
    a->mousey=target.r.y+target.r.h/2;
    render(a);
    assert(a->hover_kind==kind&&a->hover_index==index);
    assert(!strcmp(a->hover_id,id?id:""));
    return target;
}
#endif
