#include "../liljack_app/c_media.c"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s status=%s\n",__LINE__,#x,m.status);exit(1);}}while(0)
static lj_media m;
static void cleanup(void){lj_media_close(&m);}
static void pump(double secs){double end=now()+secs;while(now()<end){lj_media_poll(&m);SDL_Delay(5);}}
static void frame(void){double end=now()+8;while(!m.pixels&&!m.ended&&now()<end)pump(.01);CHECK(m.pixels);}
static void callback_output(void){
    lj_media a;lj_media_init(&a);a.audio_ring=calloc(1,PCM_CAP);
    unsigned char out[4096];int16_t samples[2048];for(int i=0;i<2048;i++)samples[i]=10000;
    memcpy(a.audio_ring,samples,sizeof(samples));a.audio_count=sizeof(samples);a.volume=100;
    audio_callback(&a,out,sizeof(out));CHECK(((int16_t*)out)[0]==10000);CHECK(a.audio_played==sizeof(out));
    a.audio_read=0;a.audio_count=sizeof(samples);lj_media_volume(&a,50);audio_callback(&a,out,sizeof(out));
    CHECK(((int16_t*)out)[0]>=4900&&((int16_t*)out)[0]<=5100);
    a.audio_read=0;a.audio_count=sizeof(samples);lj_media_mute(&a,1);audio_callback(&a,out,sizeof(out));
    for(size_t i=0;i<sizeof(out);i++)CHECK(out[i]==0);
    uint64_t consumed=a.audio_played;audio_callback(&a,out,sizeof(out));CHECK(a.audio_played==consumed);
    free(a.audio_ring);puts("PCM: amplitude scales, mute emits silence, consumed clock stops on underrun");
}
int main(int argc,char **argv){
    CHECK(argc==2);lj_media_init(&m);atexit(cleanup);callback_output();
    CHECK(lj_media_open(&m,argv[1]));frame();CHECK(m.has_audio);CHECK(fabs(m.duration-4)<.15);
    CHECK(((m.pixels[180*640+320]>>16)&255)>220);
    pump(.6);double p=lj_media_position(&m);CHECK(p>.2&&p<1.2);
    CHECK(fabs((m.frames-1)/12.0-p)<.25);
    if(!getenv("TEST_NO_AUDIO_DEVICE")){CHECK(m.audio_device);CHECK(m.audio_received>0);CHECK(m.audio_master);}
    else CHECK(m.audio_failed&&!m.audio_device);
    double slow_until=now()+.7,max_skew=0;
    while(now()<slow_until){
        lj_media_poll(&m);double skew=fabs((m.frames-1)/12.0-lj_media_position(&m));
        if(skew>max_skew)max_skew=skew;SDL_Delay(16);
    }
    printf("16ms poll: max presented-frame/clock difference %.3fs\n",max_skew);CHECK(max_skew<.3);
    lj_media_pause(&m,1);double frozen=lj_media_position(&m);uint64_t frames=m.frames;
    pump(.25);CHECK(fabs(lj_media_position(&m)-frozen)<.04);CHECK(m.frames==frames);
    lj_media_mute(&m,1);lj_media_volume(&m,30);CHECK(m.muted&&m.volume==30);
    CHECK(lj_media_seek(&m,3));frame();CHECK(m.paused&&m.muted&&m.volume==30);
    CHECK((m.pixels[180*640+320]&255)>220);CHECK(fabs(lj_media_position(&m)-3)<.05);
    pump(.15);CHECK(fabs(lj_media_position(&m)-3)<.05);
    lj_media_pause(&m,0);pump(.25);CHECK(lj_media_position(&m)>3.1);
    CHECK(lj_media_seek(&m,.5));frame();CHECK(((m.pixels[180*640+320]>>16)&255)>220);
    CHECK(lj_media_position(&m)<1);CHECK(m.muted&&m.volume==30);
    CHECK(lj_media_seek(&m,3.7));frame();double end=now()+4;while(!m.ended&&now()<end)pump(.02);
    CHECK(m.ended);CHECK(m.pid==0&&m.fd==-1&&m.audio_fd==-1);
    CHECK(lj_media_position(&m)>3.8&&lj_media_position(&m)<=4.15);
    uint32_t device=m.audio_device;lj_media_close(&m);CHECK(m.audio_device==0&&m.audio_ring==NULL);
    if(device)CHECK(SDL_GetAudioDeviceStatus(device)==SDL_AUDIO_STOPPED);
    puts("PASS controls: duration, red/blue seek truth, A/V timing, pause, seek while paused, preferences, EOF/device/child cleanup");
}
