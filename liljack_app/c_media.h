#ifndef LJ_MEDIA_H
#define LJ_MEDIA_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
typedef struct {
    uint32_t *pixels;
    int w,h,fd,resolving,ended;
    int decode_w,decode_h; /* selected video output size, independent of loaded frame */
    int fps;               /* decode/present rate; 12 (sixel over a pty) by default, owkTerm sets 24 */
    pid_t pid;
    size_t used;
    unsigned char *buffer;
    char title[160],status[160];
    char *source,*resolved;
    int probing,is_video,paused,has_audio,audio_failed,video_eof,audio_eof,decode_error;
    double duration,offset,clock_base,clock_start,pause_at,phase_start;
    uint64_t frames;
    int started,audio_started,audio_master;
    uint32_t audio_device;
    int audio_fd,audio_subsystem,muted,volume;
    unsigned char *audio_ring;
    size_t audio_read,audio_count;
    uint64_t audio_played,audio_received,audio_underruns;
} lj_media;
void lj_media_init(lj_media *m);
/* Select decode cadence before opening a source. Native-canvas hosts may use
 * 24; terminal/sixel callers retain the 12-fps default. */
int lj_media_set_fps(lj_media *m,int fps);
int lj_media_open(lj_media *m,const char *source);
int lj_media_poll(lj_media *m);
void lj_media_close(lj_media *m);
/* Position is an estimate: consumed PCM time, or monotonic time for silent
 * playback. Hardware/output latency and 12-fps presentation add uncertainty. */
double lj_media_position(lj_media *m);
void lj_media_pause(lj_media *m,int paused);
int lj_media_seek(lj_media *m,double seconds);
/* Select 360p, 480p or 720p output; preserve position and playback preferences. */
int lj_media_resolution(lj_media *m,int height);
void lj_media_mute(lj_media *m,int muted);
void lj_media_volume(lj_media *m,int percent);
void lj_media_blit(const lj_media *m,uint32_t *dst,int dw,int dh,int x,int y,int w,int h);
/* Ask a direct owkTerm host to open its native-canvas player. The source is
 * rejected if it contains terminal controls, so it cannot inject another OSC. */
int lj_media_delegate_owkterm(int fd,const char *source);
#endif
