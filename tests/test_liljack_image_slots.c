/* graphs-gaps-with-video (the operator seq 1439): with the video popup open, the header
 * GPU/CPU/MEM plots showed bigger black gaps. A frame declares images in this
 * order: popup video, one prompt mark per tile, three graphs, docked media. The
 * presenter's slot table must hold a real room's worth of them; when it did not,
 * lj_ansi_image returned 0 for the last graphs and their cells stayed black. */
#include "../liljack_app/c_ansi.c"
#include <assert.h>
static uint32_t px[64*40];
int main(void){
    int saved=dup(1);FILE *sink=tmpfile();assert(saved>=0&&sink);assert(dup2(fileno(sink),1)>=0);
    state.opened=1;state.sixel=1;state.cellw=10;state.cellh=20;
    for(int i=0;i<64*40;i++)px[i]=0xff40df80;
    int bad=0;
    for(int tiles=3;tiles<=12;tiles+=3){
        lj_ansi_begin(1280,720);lj_ansi_rect(0,0,1280,720,0x08101a);
        int ok=0,want=0;
        ok+=lj_ansi_image(400,100,640,400,px,64,40);want++;                       /* video popup */
        for(int t=0;t<tiles;t++){ok+=lj_ansi_image(20,60+t*40,20,20,px,20,20);want++;}   /* prompt marks */
        int graphs=0;for(int g=0;g<3;g++){int r=lj_ansi_image(700+g*120,0,60,20,px,60,20);ok+=r;graphs+=r;want++;}
        ok+=lj_ansi_image(1100,300,160,120,px,64,40);want++;                      /* docked media tile */
        printf("tiles=%d declared=%d accepted=%d graphs accepted=%d/3\n",tiles,want,ok,graphs);
        if(graphs!=3||ok!=want)bad++;
        assert(lj_ansi_present());
    }
    dup2(saved,1);printf("image-slots: %s\n",bad?"FAIL":"PASS");return bad?1:0;
}
