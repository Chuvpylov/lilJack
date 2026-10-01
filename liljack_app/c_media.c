#define _POSIX_C_SOURCE 200809L
#include "c_media.h"
#include "c_env.h"
#include <SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_MAX_DIMENSIONS 8192
#include "deps/stb_image.h"
static size_t frame_size(const lj_media *m){return (size_t)m->decode_w*m->decode_h*4u;}
#define PCM_RATE 48000
#define PCM_BYTES (PCM_RATE*4u)
#define PCM_CAP PCM_BYTES /* one second, stereo signed 16-bit LE */
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static void status(lj_media *m,const char *s){snprintf(m->status,sizeof(m->status),"%s",s);}
void lj_media_init(lj_media *m){memset(m,0,sizeof(*m));m->fd=m->audio_fd=-1;m->duration=-1;m->volume=70;m->decode_w=640;m->decode_h=360;m->fps=12;}
int lj_media_set_fps(lj_media *m,int fps){
    if(!m||fps<1||fps>60||m->pid>0||m->fd>=0||m->audio_fd>=0||m->resolving||m->probing)return 0;
    m->fps=fps;return 1;
}
static int write_all(int fd,const void *data,size_t n){
    const unsigned char *p=data;
    while(n){ssize_t sent=write(fd,p,n);if(sent<0){if(errno==EINTR)continue;return 0;}if(!sent)return 0;p+=(size_t)sent;n-=(size_t)sent;}
    return 1;
}
int lj_media_delegate_owkterm(int fd,const char *source){
    static const char head[]="\033]7771;play;",tail[]="\033\\";
    if(fd<0||!source||!*source)return 0;
    size_t n=strlen(source);if(n>4096)return 0;
    for(size_t i=0;i<n;i++)if((unsigned char)source[i]<32||(unsigned char)source[i]==127)return 0;
    return write_all(fd,head,sizeof(head)-1)&&write_all(fd,source,n)&&write_all(fd,tail,sizeof(tail)-1);
}
static void stop_child(pid_t pid){
    if(pid<=0||waitpid(pid,NULL,WNOHANG)!=0)return;
    kill(pid,SIGTERM);struct timespec delay={0,10000000};
    for(int i=0;i<50;i++){if(waitpid(pid,NULL,WNOHANG)!=0)return;nanosleep(&delay,NULL);}
    kill(pid,SIGKILL);waitpid(pid,NULL,0);
}
static void stop_pipeline(lj_media *m){
    if(m->audio_device)SDL_CloseAudioDevice(m->audio_device);
    m->audio_device=0;
    if(m->audio_subsystem)SDL_QuitSubSystem(SDL_INIT_AUDIO);
    m->audio_subsystem=0;
    if(m->fd>=0)close(m->fd);if(m->audio_fd>=0)close(m->audio_fd);
    m->fd=m->audio_fd=-1;stop_child(m->pid);m->pid=0;
    free(m->audio_ring);m->audio_ring=NULL;m->audio_read=m->audio_count=0;
    m->audio_played=m->audio_received=m->audio_underruns=0;
    m->audio_started=m->audio_master=m->started=0;m->used=0;
}
void lj_media_close(lj_media *m){
    stop_pipeline(m);free(m->pixels);free(m->buffer);free(m->source);free(m->resolved);lj_media_init(m);
}
/* No malloc/exec in a forked copy of the SDL audio thread. Supply the same
 * credential-free base allowlist as c_env, preserving caller CUDA settings. */
static int spawn_media(lj_media *m,char *const argv[],int with_audio){
    int p[2],a[2]={-1,-1};if(pipe(p))return 0;
    if(with_audio&&pipe(a)){close(p[0]);close(p[1]);return 0;}
    for(int i=0;i<2;i++){fcntl(p[i],F_SETFD,FD_CLOEXEC);if(a[i]>=0)fcntl(a[i],F_SETFD,FD_CLOEXEC);}
    char *env[32]={0};int count=0,err=0;
    for(int i=0;lj_env_base[i]&&count<31;i++){
        const char *v=getenv(lj_env_base[i]);if(!v)continue;
        size_t n=strlen(lj_env_base[i])+strlen(v)+2;env[count]=malloc(n);
        if(!env[count]){err=ENOMEM;break;}
        snprintf(env[count++],n,"%s=%s",lj_env_base[i],v);
    }
    posix_spawn_file_actions_t fa;posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addclose(&fa,p[0]);if(a[0]>=0)posix_spawn_file_actions_addclose(&fa,a[0]);
    posix_spawn_file_actions_adddup2(&fa,p[1],STDOUT_FILENO);
    if(p[1]!=STDOUT_FILENO)posix_spawn_file_actions_addclose(&fa,p[1]);
    if(with_audio){posix_spawn_file_actions_adddup2(&fa,a[1],3);if(a[1]!=3)posix_spawn_file_actions_addclose(&fa,a[1]);}
    posix_spawn_file_actions_addopen(&fa,STDIN_FILENO,"/dev/null",O_RDONLY,0);
    posix_spawn_file_actions_addopen(&fa,STDERR_FILENO,"/dev/null",O_WRONLY,0);
    pid_t pid=0;if(!err)err=posix_spawnp(&pid,argv[0],&fa,NULL,argv,env);
    posix_spawn_file_actions_destroy(&fa);for(int i=0;i<count;i++)free(env[i]);
    close(p[1]);if(a[1]>=0)close(a[1]);
    if(err){close(p[0]);if(a[0]>=0)close(a[0]);status(m,"Media helper unavailable · check ffmpeg/ffprobe/yt-dlp");return 0;}
    m->pid=pid;m->fd=p[0];m->audio_fd=a[0];m->used=0;m->buffer[0]=0;m->phase_start=now();
    fcntl(m->fd,F_SETFL,O_NONBLOCK);if(m->audio_fd>=0)fcntl(m->audio_fd,F_SETFL,O_NONBLOCK);
    return 1;
}
static void audio_callback(void *data,Uint8 *dst,int len){
    lj_media *m=data;memset(dst,0,(size_t)len);
    size_t wanted=(size_t)len,available=m->audio_count/4*4;
    if(wanted>available){wanted=available;m->audio_underruns++;}
    size_t left=wanted;int volume=m->muted?0:m->volume*SDL_MIX_MAXVOLUME/100;
    while(left){size_t n=PCM_CAP-m->audio_read;if(n>left)n=left;
        SDL_MixAudioFormat(dst,m->audio_ring+m->audio_read,AUDIO_S16LSB,(Uint32)n,volume);
        m->audio_read=(m->audio_read+n)%PCM_CAP;m->audio_count-=n;m->audio_played+=n;dst+=n;left-=n;
    }
}
static void audio_open(lj_media *m){
    if(!m->has_audio)return;
    if(SDL_InitSubSystem(SDL_INIT_AUDIO)!=0){m->audio_failed=1;return;}
    m->audio_subsystem=1;m->audio_ring=malloc(PCM_CAP);
    SDL_AudioSpec want={0};want.freq=PCM_RATE;want.format=AUDIO_S16LSB;want.channels=2;
    want.samples=1024;want.callback=audio_callback;want.userdata=m;
    if(m->audio_ring)m->audio_device=SDL_OpenAudioDevice(NULL,0,&want,NULL,0);
    if(!m->audio_device)m->audio_failed=1;
}
static int start_decoder(lj_media *m){
    stop_pipeline(m);m->resolving=m->probing=0;m->ended=m->video_eof=m->decode_error=0;
    m->audio_eof=!m->has_audio;m->audio_failed=0;m->frames=0;m->clock_base=m->offset;
    char seek[64],filter[192];snprintf(seek,sizeof(seek),"%.6f",m->offset);
    snprintf(filter,sizeof(filter),"scale=%d:%d:force_original_aspect_ratio=decrease,pad=%d:%d:(ow-iw)/2:(oh-ih)/2,fps=%d:start_time=0",
             m->decode_w,m->decode_h,m->decode_w,m->decode_h,m->fps>0?m->fps:12);
    char *args[]={"ffmpeg","-nostdin","-loglevel","error","-threads","2","-filter_threads","1",
        "-rw_timeout","10000000","-ss",seek,"-i",m->resolved,
        "-map","0:v:0","-vf",filter,
        "-pix_fmt","bgra","-threads","2","-f","rawvideo","pipe:1",
        "-map","0:a:0","-af","aresample=async=1:first_pts=0","-ac","2","-ar","48000",
        "-threads","1","-f","s16le","pipe:3",NULL};
    if(!m->has_audio)args[25]=NULL; /* omit the second output, including -map */
    if(!spawn_media(m,args,m->has_audio)){m->ended=1;return 0;}
    audio_open(m);status(m,m->offset>0?"Seeking…":"Loading video…");return 1;
}
static int start_probe(lj_media *m){
    stop_pipeline(m);m->resolving=0;m->probing=1;
    char *args[]={"ffprobe","-v","error","-threads","1","-rw_timeout","10000000",
        "-show_entries","format=duration:stream=codec_type","-of","default=noprint_wrappers=1",m->resolved,NULL};
    status(m,"Reading media duration…");return spawn_media(m,args,0);
}
static int metadata_poll(lj_media *m){
    if(now()-m->phase_start>20){stop_pipeline(m);m->ended=1;status(m,"Media unavailable · lookup timed out");return 1;}
    int eof=0;
    for(int i=0;i<8;i++){
        ssize_t n=read(m->fd,m->buffer+m->used,65536-m->used);
        if(n<0){if(errno==EAGAIN||errno==EINTR)break;eof=1;break;}
        if(!n){eof=1;break;}m->used+=(size_t)n;m->buffer[m->used]=0;
        if(m->used==65536){stop_pipeline(m);m->ended=1;status(m,"Media metadata exceeded limit");return 1;}
    }
    if(!eof)return 0;
    int result=0;if(waitpid(m->pid,&result,WNOHANG)!=m->pid)return 0;
    m->pid=0;close(m->fd);m->fd=-1;
    if(!WIFEXITED(result)||WEXITSTATUS(result)!=0){m->ended=1;status(m,"Media unavailable · check source and media helpers");return 1;}
    char *s=(char*)m->buffer;
    if(m->resolving){
        char *nl=strpbrk(s,"\r\n");if(nl)*nl=0;
        if(strncmp(s,"https://",8)&&strncmp(s,"http://",7)){m->ended=1;status(m,"Resolver returned unsupported media");return 1;}
        free(m->resolved);m->resolved=strdup(s);
        if(!m->resolved||!start_probe(m))m->ended=1;
    }else{
        m->has_audio=strstr(s,"codec_type=audio")!=NULL;
        char *d=strstr(s,"duration=");m->duration=d?strtod(d+9,NULL):-1;
        if(!isfinite(m->duration)||m->duration<=0)m->duration=-1;
        start_decoder(m);
    }
    return 1;
}
int lj_media_open(lj_media *m,const char *src){
    /* Copy first: callers may reopen the owned original URL after expiry. */
    char *source=strdup(src?src:"");if(!source)return 0;
    lj_media_close(m);m->source=source;src=source;
    int remote=!strncmp(src,"https://",8)||!strncmp(src,"http://",7);
    if(strstr(src,"://")&&!remote){status(m,"Only local files and HTTP(S) media are supported");return 0;}
    const char *name=strrchr(src,'/');snprintf(m->title,sizeof(m->title),"%s",remote?"Public media":name?name+1:src);
    const char *ext=strrchr(src,'.');
    if(!remote&&ext&&(!strcasecmp(ext,".png")||!strcasecmp(ext,".jpg")||!strcasecmp(ext,".jpeg")||!strcasecmp(ext,".gif")||!strcasecmp(ext,".bmp")||!strcasecmp(ext,".tga"))){
        struct stat st;if(stat(src,&st)||st.st_size>20000000){status(m,"Image missing or larger than 20 MB");return 0;}
        int c;
        if(!stbi_info(src,&m->w,&m->h,&c)||(size_t)m->w*m->h>16000000){status(m,"Image invalid or exceeds 16 megapixels");return 0;}
        unsigned char *data=stbi_load(src,&m->w,&m->h,&c,4);
        if(!data){status(m,"Image could not be decoded");return 0;}
        size_t n=(size_t)m->w*m->h;
        if(n>16000000){stbi_image_free(data);status(m,"Image exceeds 16 megapixels");return 0;}
        m->pixels=malloc(n*4);
        if(!m->pixels){stbi_image_free(data);return 0;}
        for(size_t i=0;i<n;i++)m->pixels[i]=((uint32_t)data[i*4+3]<<24)|(data[i*4]<<16)|(data[i*4+1]<<8)|data[i*4+2];
        stbi_image_free(data);status(m,"Image · drop another file to replace");return 1;
    }

    m->is_video=1;m->buffer=malloc(frame_size(m)+1);if(!m->buffer)return 0;
    const char *host=remote?strstr(src,"://")+3:"";
    int yt=!strncmp(host,"youtube.com/",12)||!strncmp(host,"www.youtube.com/",16)||!strncmp(host,"m.youtube.com/",14)||!strncmp(host,"youtu.be/",9);
    if(yt){
        m->resolving=1;status(m,"Resolving public video…");
        char *args[]={"yt-dlp","--no-playlist","--no-warnings","--socket-timeout","10","--retries","1",
            "-g","-f","best[height<=480]/best","--",m->source,NULL};
        return spawn_media(m,args,0);
    }
    m->resolved=strdup(src);return m->resolved&&start_probe(m);
}
static void audio_stats(lj_media *m,uint64_t *played,size_t *queued){
    if(m->audio_device)SDL_LockAudioDevice(m->audio_device);
    *played=m->audio_played;*queued=m->audio_count;
    if(m->audio_device)SDL_UnlockAudioDevice(m->audio_device);
}
double lj_media_position(lj_media *m){
    double p=m->clock_base;
    if(m->audio_master){uint64_t played;size_t queued;audio_stats(m,&played,&queued);p=m->offset+played/(double)PCM_BYTES;}
    else if(m->started)p+=(m->paused?m->pause_at:now())-m->clock_start;
    if(p<0)p=0;if(m->duration>0&&p>m->duration)p=m->duration;return p;
}
void lj_media_pause(lj_media *m,int paused){
    paused=!!paused;if(paused==m->paused||!m->is_video)return;
    double t=now();
    if(paused)m->pause_at=t;else if(m->started)m->clock_start+=t-m->pause_at;
    m->paused=paused;if(m->audio_device&&m->audio_started)SDL_PauseAudioDevice(m->audio_device,paused);
}
void lj_media_mute(lj_media *m,int muted){
    if(m->audio_device)SDL_LockAudioDevice(m->audio_device);m->muted=!!muted;
    if(m->audio_device)SDL_UnlockAudioDevice(m->audio_device);
}
void lj_media_volume(lj_media *m,int percent){
    if(m->audio_device)SDL_LockAudioDevice(m->audio_device);m->volume=percent<0?0:percent>100?100:percent;
    if(m->audio_device)SDL_UnlockAudioDevice(m->audio_device);
}
int lj_media_seek(lj_media *m,double seconds){
    if(!m->is_video||m->resolving||m->probing||!m->resolved||!isfinite(seconds))return 0;
    if(seconds<0)seconds=0;if(m->duration>0&&seconds>m->duration)seconds=m->duration;
    m->offset=seconds;free(m->pixels);m->pixels=NULL;m->w=m->h=0;
    return start_decoder(m);
}
int lj_media_resolution(lj_media *m,int height){
    int width=height==360?640:height==480?854:height==720?1280:0;
    if(!width||!m->is_video||!m->buffer)return 0;
    if(m->decode_h==height)return 1;
    /* Allocate before interrupting playback so allocation failure keeps it intact. */
    size_t bytes=(size_t)width*height*4u;
    unsigned char *buffer=malloc(bytes+1);if(!buffer)return 0;
    double position=lj_media_position(m);
    if(m->resolving||m->probing){
        memcpy(buffer,m->buffer,m->used+1);free(m->buffer);m->buffer=buffer;
        m->decode_w=width;m->decode_h=height;return 1;
    }
    if(!m->resolved){free(buffer);return 0;}
    stop_pipeline(m);free(m->buffer);m->buffer=buffer;
    free(m->pixels);m->pixels=NULL;m->w=m->h=0;
    m->decode_w=width;m->decode_h=height;m->offset=position;
    return start_decoder(m);
}
static void audio_poll(lj_media *m){
    if(m->audio_fd<0)return;
    unsigned char buf[8192];
    for(int i=0;i<8;i++){
        uint64_t played;size_t queued;audio_stats(m,&played,&queued);
        size_t cap=m->audio_device?PCM_CAP-queued:sizeof(buf);if(cap>sizeof(buf))cap=sizeof(buf);if(!cap)break;
        ssize_t n=read(m->audio_fd,buf,cap);
        if(n<0){if(errno==EAGAIN||errno==EINTR)break;n=0;}
        if(!n){close(m->audio_fd);m->audio_fd=-1;m->audio_eof=1;break;}
        if(m->audio_device){
            SDL_LockAudioDevice(m->audio_device);
            size_t at=(m->audio_read+m->audio_count)%PCM_CAP,first=PCM_CAP-at;if(first>(size_t)n)first=(size_t)n;
            memcpy(m->audio_ring+at,buf,first);memcpy(m->audio_ring,buf+first,(size_t)n-first);
            m->audio_count+=(size_t)n;m->audio_received+=(size_t)n;SDL_UnlockAudioDevice(m->audio_device);
        }
    }
}
int lj_media_poll(lj_media *m){
    if(m->ended||!m->is_video)return 0;
    if(m->resolving||m->probing)return m->fd>=0?metadata_poll(m):0;
    if(m->paused&&m->pixels)return 0;
    int changed=0;audio_poll(m);
    uint64_t played;size_t queued;audio_stats(m,&played,&queued);
    if(m->audio_device&&m->pixels&&!m->audio_started&&queued>=4){
        m->audio_started=m->audio_master=1;SDL_PauseAudioDevice(m->audio_device,m->paused);
    }
    if(m->audio_master&&m->audio_eof&&queued<4){
        m->clock_base=m->offset+played/(double)PCM_BYTES;m->clock_start=now();m->audio_master=0;
    }
    /* Linux may expose only 8 KiB per pipe read. Yield briefly to the writer
     * while assembling a frame instead of stretching 900 KiB across dozens
     * of UI ticks. Total drain time is capped; never wait for the next frame. */
    double drain_until=now()+.004;
    for(int i=0;i<256&&m->fd>=0&&now()<drain_until;i++){
        if(m->used==frame_size(m)){
            if(m->pixels&&(m->paused||m->frames/(double)(m->fps>0?m->fps:12)>lj_media_position(m)-m->offset+.005))break;
            if(!m->pixels)m->pixels=malloc(frame_size(m));
            if(!m->pixels){status(m,"Video allocation failed");m->ended=1;return 1;}
            memcpy(m->pixels,m->buffer,frame_size(m));m->w=m->decode_w;m->h=m->decode_h;m->used=0;m->frames++;changed=1;
            if(!m->started){m->started=1;m->clock_start=now();m->pause_at=m->clock_start;}
            if(m->paused)break;
        }
        ssize_t n=read(m->fd,m->buffer+m->used,frame_size(m)-m->used);
        if(n<0){
            if(errno==EINTR)continue;
            if(errno==EAGAIN){
                if(m->used&&now()<drain_until){struct pollfd p={m->fd,POLLIN,0};poll(&p,1,1);continue;}
                break;
            }
            n=0;
        }
        if(!n){close(m->fd);m->fd=-1;m->video_eof=1;break;}m->used+=(size_t)n;
    }
    if(m->pid>0){int result=0;if(waitpid(m->pid,&result,WNOHANG)==m->pid){
        m->pid=0;m->decode_error=!WIFEXITED(result)||WEXITSTATUS(result)!=0;changed=1;
    }}
    audio_stats(m,&played,&queued);
    if(m->video_eof&&m->audio_eof&&!m->pid&&(!m->audio_device||queued<4)){
        m->clock_base=lj_media_position(m);m->started=m->audio_master=0;m->ended=1;
        if(m->audio_device)SDL_PauseAudioDevice(m->audio_device,1);
        status(m,m->decode_error||!m->pixels?"Media unavailable · check source and ffmpeg":"Video ended");changed=1;
    }else if(m->pixels)status(m,m->audio_failed?"Playing · audio device unavailable":!m->has_audio?"Playing · no audio track":"Playing");
    return changed;
}
void lj_media_blit(const lj_media *m,uint32_t *dst,int dw,int dh,int x,int y,int w,int h){
    if(!m->pixels||w<1||h<1||m->w<1||m->h<1)return;
    double scale=(double)w/m->w;if((double)h/m->h<scale)scale=(double)h/m->h;
    int rw=(int)(m->w*scale),rh=(int)(m->h*scale);x+=(w-rw)/2;y+=(h-rh)/2;
    for(int yy=0;yy<rh;yy++)for(int xx=0;xx<rw;xx++){
        if(x+xx<0||x+xx>=dw||y+yy<0||y+yy>=dh)continue;
        uint32_t p=m->pixels[(size_t)(yy/scale)*m->w+(int)(xx/scale)];
        unsigned a=p>>24;uint32_t *d=&dst[(size_t)(y+yy)*dw+x+xx],b=*d;
        unsigned r=(((p>>16)&255)*a+((b>>16)&255)*(255-a))/255;
        unsigned g=(((p>>8)&255)*a+((b>>8)&255)*(255-a))/255;
        unsigned bl=((p&255)*a+(b&255)*(255-a))/255;*d=0xff000000|(r<<16)|(g<<8)|bl;
    }
}
