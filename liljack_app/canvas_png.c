/* canvas-png ROOT ROOM OUT.png W H [HIDDEN]: the room canvas rendered by the same code
 * as the canvas tile, for agents (`liljack room --canvas-png`). */
#include "c_canvas.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char **argv){
    if(argc!=6&&argc!=7){fprintf(stderr,"usage: canvas-png ROOT ROOM OUT.png W H [HIDDEN_AUTHOR_BITS]\n");return 2;}
    int w=atoi(argv[4]),h=atoi(argv[5]);
    if(w<16||h<16||w>8192||h>8192||(long)w*h>16000000L){fprintf(stderr,"canvas-png: sides 16..8192, at most 16M pixels\n");return 2;}
    lj_canvas c;lj_canvas_init(&c);
    if(!lj_canvas_open(&c,argv[1],argv[2])){fprintf(stderr,"canvas-png: %s\n",c.status);return 1;}
    if(argc==7)c.hidden=(unsigned)strtoul(argv[6],NULL,10)&31u;   /* per-agent view: hide the others */
    int ok=lj_canvas_save_png(&c,argv[3],w,h,0x101418);
    if(!ok)fprintf(stderr,"canvas-png: %s\n",c.status);
    lj_canvas_close(&c);return ok?0:1;
}
