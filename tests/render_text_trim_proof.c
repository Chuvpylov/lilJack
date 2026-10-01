#include "c_render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define W 200
#define H 60
static uint32_t *snap(void){
    uint32_t *p=lj_render_pixels(); size_t n=(size_t)W*H;
    uint32_t *c=malloc(n*4); memcpy(c,p,n*4); return c;
}
static long ink(uint32_t *b){ long k=0; for(size_t i=0;i<(size_t)W*H;i++) if((b[i]&0xffffff)>0x101010) k++; return k; }
int main(void){
    if(lj_render_init(W,H)<0){puts("no faces");return 2;}
    long inks[2]; int identical[2];
    for(int t=0;t<2;t++){
        int trim = t? 4 : 0;
        lj_render_text_trim(trim);
        lj_render_rect(0,0,W,H,0x000000); lj_render_text(10,10,"AB",0xffffff,0);
        uint32_t *both=snap();
        lj_render_rect(0,0,W,H,0x000000);
        lj_render_text(10,10,"A",0xffffff,0);
        lj_render_text(10+LJ_CELL_W,10,"B",0xffffff,0);
        uint32_t *split=snap();
        identical[t] = memcmp(both,split,(size_t)W*H*4)==0;
        inks[t]=ink(both);
        free(both); free(split);
    }
    printf("trim=0  ink=%ld  advance_is_exactly_LJ_CELL_W=%s\n", inks[0], identical[0]?"YES":"NO");
    printf("trim=4  ink=%ld  advance_is_exactly_LJ_CELL_W=%s\n", inks[1], identical[1]?"YES":"NO");
    printf("text got smaller: %s (%ld -> %ld)\n", inks[1]<inks[0]?"YES":"NO", inks[0], inks[1]);
    lj_render_text_trim(0);
    lj_render_close();
    return (identical[0]&&identical[1]&&inks[1]<inks[0])?0:1;
}
