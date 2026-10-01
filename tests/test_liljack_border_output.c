/* Actual presenter bytes, isolated stdout sink; no live terminal or backend. */
#define lj_sixel_encode counted_sixel_encode
#include "../liljack_app/c_ansi.c"
#undef lj_sixel_encode
#include <assert.h>
static int encodes;
extern int lj_sixel_encode(const uint32_t *,int,int,int,int,int,int,char *,int);
int counted_sixel_encode(const uint32_t *p,int fw,int fh,int x,int y,int w,int h,char *out,int cap){
    encodes++;return lj_sixel_encode(p,fw,fh,x,y,w,h,out,cap);
}
static FILE *sink;
static int saved_stdout,failures;
static off_t at(void){off_t p=lseek(STDOUT_FILENO,0,SEEK_CUR);assert(p>=0);return p;}
static int dcs_since(off_t start){
    off_t end=at();size_t n=(size_t)(end-start);char *p=malloc(n+1);assert(p);
    assert(pread(STDOUT_FILENO,p,n,start)==(ssize_t)n);int count=0;
    for(size_t i=0;i+1<n;i++)count+=p[i]==27&&p[i+1]=='P';free(p);return count;
}
static void frame(int shift,uint32_t gold,int damage){
    lj_ansi_begin(800,560);lj_ansi_rect(0,0,800,560,0x101010);
    if(damage)lj_ansi_glyph(damage==1?10:100,damage==1?20:100,'X',0xffffff,1);
    assert(lj_ansi_border(20+shift,40,300,160,gold,0x00ffff,0));
    lj_ansi_separator(600,40,10,160,0xffcc00,1);
}
static void check(const char *name,int actual,int expected){
    dprintf(saved_stdout,"%s actual=%d expected=%d %s\n",name,actual,expected,actual==expected?"PASS":"FAIL");
    failures+=actual!=expected;
}
int main(void){
    saved_stdout=dup(STDOUT_FILENO);assert(saved_stdout>=0);sink=tmpfile();assert(sink);
    assert(dup2(fileno(sink),STDOUT_FILENO)>=0);
    state.opened=1;state.sixel=1;state.cellw=10;state.cellh=20;
    off_t start=at();frame(0,0xffcc00,0);assert(lj_ansi_present());
    check("initial ring plus separator DCS",dcs_since(start),5);
    size_t present_bytes=0,tick_bytes=0;int presents=0,ticks=0,present_encodes=0,tick_encodes=0;
    for(int i=1;i<=40;i++){
        int before=encodes;
        start=at();frame(0,0xffcc00,0);assert(lj_ansi_present());
        present_encodes+=encodes-before;before=encodes;
        present_bytes+=(size_t)(at()-start);presents+=dcs_since(start);
        start=at();assert(lj_ansi_border_tick((uint32_t)i));
        tick_bytes+=(size_t)(at()-start);ticks+=dcs_since(start);
        tick_encodes+=encodes-before;
    }
    dprintf(saved_stdout,"40 presents bytes=%zu; 40 ticks bytes=%zu; 20 ticks/s budget=%.0f bytes/s\n",
            present_bytes,tick_bytes,tick_bytes/2.0);
    check("idle present bytes",(int)present_bytes,0);
    check("idle present DCS",presents,0);check("tick DCS",ticks,160);
    check("idle present encodes",present_encodes,0);check("tick strip encodes",tick_encodes,160);
    start=at();assert(lj_ansi_border_tick(40));check("same tick bytes",(int)(at()-start),0);
    start=at();frame(0,0xffcc00,1);assert(lj_ansi_present());check("gutter text damage DCS",dcs_since(start),4);
    start=at();assert(lj_ansi_border_tick(40));check("repair preserves last tick bytes",(int)(at()-start),0);
    /* Repair clearing X, then an interior text change must leave the ring alone. */
    frame(0,0xffcc00,0);assert(lj_ansi_present());
    start=at();frame(0,0xffcc00,2);assert(lj_ansi_present());check("interior text DCS",dcs_since(start),0);
    int before=encodes;
    start=at();frame(10,0xffcc00,2);assert(lj_ansi_present());check("moved ring DCS",dcs_since(start),4);
    check("moved ring strip encodes",encodes-before,4);
    start=at();frame(10,0xeeee00,2);assert(lj_ansi_present());check("changed colour DCS",dcs_since(start),4);
    /* An image overlapping the gutter needs the ring restored after it. */
    uint32_t pixel=0xff334455;
    start=at();frame(10,0xeeee00,2);
    assert(lj_ansi_image(20,20,10,20,&pixel,1,1));assert(lj_ansi_present());
    check("image damage DCS",dcs_since(start),5);
    start=at();frame(10,0xeeee00,2);
    assert(lj_ansi_image(20,20,10,20,&pixel,1,1));assert(lj_ansi_present());
    check("unchanged image and ring bytes",(int)(at()-start),0);
    /* Change the host pixel grid; cached bytes must use the new cell size. */
    state.cellw=8;state.cellh=16;before=encodes;
    frame(10,0xeeee00,2);assert(lj_ansi_present());
    check("host resize strip encodes",encodes-before,5);
    /* Close while the cache owns wires, including an abandoned frame. */
    lj_ansi_begin(800,560);lj_ansi_begin(800,560);state.opened=0;lj_ansi_close();lj_ansi_close();
    assert(dup2(saved_stdout,STDOUT_FILENO)>=0);close(saved_stdout);fclose(sink);
    return failures?1:0;
}
