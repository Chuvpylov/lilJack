/* Synthetic lab signal through the production copier, scaler and sixel writer.
 * Reuse the independent decoder from the encoder's existing round-trip test. */
#include "../liljack_app/c_ansi.c"
#define decode graph_decode
#define main sixel_existing_test_main
#include "test_liljack_sixel.c"
#undef main
#undef decode
#include <math.h>
#include <assert.h>
static int dcs_count(off_t from){
    off_t end=lseek(1,0,SEEK_CUR);assert(end>=from);size_t n=(size_t)(end-from);
    char *p=malloc(n+1);assert(p);assert(pread(1,p,n,from)==(ssize_t)n);int count=0;
    for(size_t i=0;i+1<n;i++)count+=p[i]==27&&p[i+1]=='P';free(p);return count;
}
static void gate_frame(uint32_t *src,int damage){
    lj_ansi_begin(800,560);lj_ansi_rect(0,0,800,560,0x08101a);
    if(damage)lj_ansi_glyph(20,20,'X',0xffffff,1);
    assert(lj_ansi_image(10,20,80,20,src,80,20));
}
int main(void){
    int saved=dup(1);FILE *sink=tmpfile();assert(saved>=0&&sink);assert(dup2(fileno(sink),1)>=0);
    state.opened=1;state.sixel=1;state.cellw=17;state.cellh=39;
    char *wire=malloc(SIXEL_CAP);uint32_t *scaled=NULL;size_t cap=0;
    const int sizes[]={47,75,112};int failures=0;
    for(int z=0;z<3;z++){
        int sw=(sizes[z]/4-2)*10,sh=40,tw=sw/10*17,th=78;
        uint32_t *src=malloc((size_t)sw*sh*4),*decoded=malloc((size_t)tw*th*4);
        int missing=0,mismatched=0;
        for(int f=0;f<300;f++)for(int k=0;k<4;k++){
            for(int i=0;i<sw*sh;i++)src[i]=0xff102030;
            for(int x=0;x<sw;x++){
                float v=.50f+.32f*sinf((x+f)*.11f+k)+.08f*sinf((x+f)*.37f);
                int y=sh-2-(int)(v*(sh-4));src[y*sw+x]=src[(y+1)*sw+x]=0xff40df80;
            }
            lj_ansi_begin(sizes[z]*10,360);
            assert(lj_ansi_image(10,20,sw,sh,src,sw,sh));
            memset(src,0,(size_t)sw*sh*4); /* caller may immediately reuse it */
            assert(lseek(1,0,SEEK_SET)==0);assert(ftruncate(1,0)==0);
            int n=emit_image(&state.images[0],0,wire,&scaled,&cap);assert(n>0);
            graph_decode(wire,n,decoded,tw,th);
            for(int x=0;x<tw;x++){
                int ink=0;
                for(int y=0;y<th;y++){
                    uint32_t want=scaled[y*tw+x]&0xffffff;
                    int r=(((want>>16)&255)*100/255*255+50)/100;
                    int g=(((want>>8)&255)*100/255*255+50)/100;
                    int b=((want&255)*100/255*255+50)/100;
                    mismatched+=decoded[y*tw+x]!=(uint32_t)((r<<16)|(g<<8)|b);
                    ink+=((decoded[y*tw+x]>>8)&255)>140;
                }
                missing+=!ink;
            }
        }
        dprintf(saved,"cols=%d source=%dx%d target=%dx%d images=1200 missing_columns=%d mismatched_pixels=%d\n",sizes[z],sw,sh,tw,th,missing,mismatched);
        failures+=missing||mismatched;free(src);free(decoded);
    }
    state.opened=0;lj_ansi_close();
    state.opened=1;state.sixel=1;state.cellw=17;state.cellh=39;state.min_ms=100000;
    uint32_t src[80*20];for(int i=0;i<80*20;i++)src[i]=0xff102030;
    for(int x=0;x<80;x++)src[19*80+x]=0xff40df80;
    off_t at=lseek(1,0,SEEK_CUR);gate_frame(src,0);assert(lj_ansi_present());assert(dcs_count(at)==1);
    at=lseek(1,0,SEEK_CUR);gate_frame(src,0);assert(lj_ansi_present());assert(dcs_count(at)==0);
    /* One changed source pixel is hashed in the full 1600px header image.
     * Throttling holds the old whole image; its shown hash stays unchanged. */
    unsigned long long held=state.shown[0];src[18*80+2]=0xff40df80;
    at=lseek(1,0,SEEK_CUR);gate_frame(src,0);assert(state.images[0].hash!=held);
    assert(lj_ansi_present());assert(dcs_count(at)==0&&state.shown[0]==held);
    state.shown_at[0]=milliseconds()-state.min_ms-1;
    at=lseek(1,0,SEEK_CUR);gate_frame(src,0);assert(lj_ansi_present());assert(dcs_count(at)==1&&state.shown[0]!=held);
    /* Damage repairs bypass the content throttle even if pixels are unchanged. */
    at=lseek(1,0,SEEK_CUR);gate_frame(src,1);assert(lj_ansi_present());assert(dcs_count(at)==1);
    dprintf(saved,"header gates: unchanged=0 DCS; throttled=0 unchanged shown hash; expired=1 DCS; damaged=1 DCS PASS\n");
    state.opened=0;lj_ansi_close();free(scaled);free(wire);dup2(saved,1);close(saved);fclose(sink);
    return failures?1:0;
}
