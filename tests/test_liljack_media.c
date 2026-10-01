#define _POSIX_C_SOURCE 200809L
#include "../liljack_app/c_media.h"
#include "../liljack_app/c_render.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static lj_media media;
static const char *image_path;
#define CHECK(c) do {if(!(c)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
static void cleanup(void){lj_media_close(&media);lj_render_close();if(image_path)remove(image_path);}
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static void pump(void){struct timespec pause={0,5000000};lj_media_poll(&media);nanosleep(&pause,NULL);}
static void ended(void){double until=now()+8;while(!media.ended&&now()<until)pump();CHECK(media.ended);}
static void child_reaped(pid_t pid){errno=0;CHECK(waitpid(pid,NULL,WNOHANG)==-1&&errno==ECHILD);}
int main(int argc,char **argv){
    CHECK(argc==3);lj_media_init(&media);CHECK(atexit(cleanup)==0);image_path=argv[2];
    CHECK(lj_render_init(16,8)==0);lj_render_rect(0,0,16,8,0xff0000);lj_render_rect(8,0,8,8,0x00ff00);CHECK(lj_render_save_png(image_path)==0);
    CHECK(lj_media_open(&media,image_path));CHECK(media.w==16&&media.h==8&&media.fd==-1&&media.pid==0);
    CHECK(media.pixels[0]==0xffff0000&&media.pixels[15]==0xff00ff00);
    uint32_t pixels[32*32];for(size_t i=0;i<32*32;i++)pixels[i]=0xff112233;
    lj_media_blit(&media,pixels,32,32,4,4,24,24);
    /* 2:1 input becomes 24x12, centered inside the square target. */
    CHECK(pixels[9*32+4]==0xff112233);CHECK(pixels[10*32+4]==0xffff0000);CHECK(pixels[10*32+27]==0xff00ff00);CHECK(pixels[22*32+4]==0xff112233);
    lj_media_blit(&media,pixels,32,32,-12,-12,24,24); /* clipped destination */
    lj_media_blit(&media,pixels,32,32,0,0,0,0);
    lj_media_close(&media);CHECK(!media.pixels&&media.fd==-1&&media.pid==0);
    CHECK(!lj_media_open(&media,"ftp://example.invalid/video"));CHECK(media.pid==0&&media.fd==-1);
    CHECK(!lj_media_open(&media,"file:///etc/passwd"));CHECK(media.pid==0&&media.fd==-1);
    FILE *bad=fopen(image_path,"w");CHECK(bad);CHECK(fputs("not an image",bad)>=0);CHECK(!fclose(bad));CHECK(!lj_media_open(&media,image_path));
    CHECK(lj_media_open(&media,argv[1]));pid_t decoder=media.pid;int fd=media.fd;CHECK(decoder>0&&fd>=0);
    double until=now()+8;while(!media.pixels&&!media.ended&&now()<until)pump();CHECK(media.pixels&&media.w==640&&media.h==360);
    /* Fixture is solid red: decoded RGB tolerates YUV codec rounding. */
    uint32_t center=media.pixels[180*640+320];CHECK(((center>>16)&255)>220);CHECK(((center>>8)&255)<30);CHECK((center&255)<30);
    ended();CHECK(media.pid==0&&media.fd==-1);child_reaped(decoder);errno=0;CHECK(fcntl(fd,F_GETFD)==-1&&errno==EBADF);
    CHECK(strstr(media.status,"ended")!=NULL);lj_media_close(&media);
    /* Early close must terminate and reap an active decoder too. */
    CHECK(lj_media_open(&media,argv[1]));decoder=media.pid;fd=media.fd;lj_media_close(&media);child_reaped(decoder);errno=0;CHECK(fcntl(fd,F_GETFD)==-1&&errno==EBADF);
    CHECK(lj_media_open(&media,"/nonexistent/liljack-native-test-missing.mp4"));ended();CHECK(!media.pixels);CHECK(strstr(media.status,"unavailable")!=NULL);
    puts("C media: PNG pixels, aspect fit/clipping, local video frame/end, child/fd cleanup and invalid inputs passed");return 0;
}
