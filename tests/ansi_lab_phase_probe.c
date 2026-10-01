/* Read-only production-encoder/VT-decoder probe; reports phase continuity. */
#include "../liljack_app/c_ansi.c"
#include "../liljack_app/owkterm_vt.h"
#include <assert.h>
static uint32_t tiles[4096][17*39],next;
static void reset(void*u,uint32_t id,uint32_t bg){(void)u;(void)bg;memset(tiles[id],0,sizeof tiles[id]);}
static uint32_t create(void*u,uint32_t bg){assert(++next<4096);reset(u,next,bg);return next;}
static void plot(void*u,uint32_t id,int x,int y,uint32_t rgb){(void)u;tiles[id][y*17+x]=rgb;}
static uint32_t sample(void *vt,int x,int y){const lj_cell*c=lj_vt_row(vt,y/39)+x/17;return c->marks[1]?tiles[c->marks[1]][(y%39)*17+x%17]:0;}
int main(void){
    state.opened=1;state.sixel=1;state.cellw=17;state.cellh=39;
    lj_ansi_begin(800,560);lj_ansi_rect(0,0,800,560,0x08101a);
    assert(lj_ansi_border(20,40,300,160,0xffcf40,0x4eb7ff,0));
    pixel_border*b=&state.borders[0];int flips=0;
    unsigned phases[]={4,5,6,7,10,11,12,13};uint32_t previous=0;
    for(unsigned i=0;i<sizeof phases/sizeof phases[0];i++){
        b->phase=phases[i];assert(prepare_border(b));next=0;
        void*vt=lj_vt_new(80,28);lj_vt_pixel_sink sink={NULL,17,39,create,plot,reset};lj_vt_set_pixel_sink(vt,&sink);
        lj_vt_feed(vt,(unsigned char*)b->wire,(int)b->bytes);
        int x=b->col*17+(b->inner_col?16:0)+(int)phases[i];
        int y=b->row*39+(b->inner_row?38:0);
        uint32_t color=sample(vt,x,y);assert(color);
        int changed=previous&&previous!=color;flips+=changed;
        printf("phase=%u tracked_dot_x=%d rgb=%06x changed=%d\n",phases[i],x,color,changed);
        previous=color;lj_vt_free(vt);
    }
    printf("tracked dot changes=%d (persistent color expects 0)\n",flips);
    state.opened=0;lj_ansi_close();return flips?1:0;
}
