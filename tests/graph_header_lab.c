/* Actual header/controller in an owned terminal, deterministic completed history.
 * No metrics acquisition, backend, session spawn, or user state. */
#define main liljack_entry
#include "../liljack_app/main.c"
#undef main
#include "../liljack_app/c_metrics.c"
int main(int argc,char **argv){
    int seconds=25;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--seconds")&&i+1<argc)seconds=atoi(argv[++i]);
        else return 2;
    }
    if(seconds<1||seconds>60)return 2;
    setlocale(LC_ALL,"");App *a=calloc(1,sizeof *a);if(!a)return 1;
    a->running=1;a->demo=1;a->ansi=1;a->backend_in=a->backend_out=-1;a->split=-1;a->mousex=a->mousey=-1;
    for(int i=0;i<COUNT;i++)a->sessions[i].fd=-1;
    lj_dock_init(&a->dock);lj_media_init(&a->media);demo(a);
    if(!lj_ansi_open(&a->w,&a->h)){free(a);return 1;}
    int cw,ch;lj_ansi_cell_pixels(&cw,&ch);
    fprintf(stderr,"ansi-lab cols=%d rows=%d cell=%dx%d fixture=actual-header graphs=%d sixel=%d\n",a->w/CELLW,a->h/CELLH,cw,ch,header_layout(a).rung==2?3:0,lj_ansi_images_available());
    int pw=LJ_RIBBON_SPARK*CELLW;G.display_count=pw+20;
    for(int m=0;m<LJ_METRIC_COUNT;m++)for(int i=0;i<G.display_count;i++){
        int x=i-20,c=x/CELLW;
        G.display[m][i]=x<0?1.f:c==0?0.f:c==1?.001f:c==2?NAN:(float)(x-3*CELLW)/(pw-3*CELLW-1);
    }
    G.paced_started=0;
    uint64_t start=SDL_GetTicks64();
    while(SDL_GetTicks64()-start<(uint64_t)seconds*1000){
        SDL_Event e;while(lj_ansi_poll(&e)){if(e.type==SDL_USEREVENT)free(e.user.data1);}
        render(a);if(!lj_ansi_present())break;
        struct timespec delay={0,100000000};nanosleep(&delay,NULL);
    }
    lj_ansi_close();for(int i=0;i<a->nsession;i++)terminal_close(&a->sessions[i]);
    lj_media_close(&a->media);free(a);return 0;
}
