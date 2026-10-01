#define _POSIX_C_SOURCE 200809L
#include "../liljack_app/c_media.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static size_t capture(const char *source,char *out,size_t cap){
    int p[2];assert(pipe(p)==0);
    int ok=lj_media_delegate_owkterm(p[1],source);close(p[1]);
    ssize_t n=read(p[0],out,cap-1);close(p[0]);assert(n>=0);out[n]=0;
    return ok?(size_t)n:0;
}

int main(void){
    char out[512];const char *url="https://example.test/video?id=1";
    size_t n=capture(url,out,sizeof out);
    char expected[512];snprintf(expected,sizeof expected,"\033]7771;play;%s\033\\",url);
    assert(n==strlen(expected)&&!memcmp(out,expected,n));

    /* A source can otherwise terminate OSC 7771 and inject arbitrary terminal
     * commands. Rejection must happen before a single byte is emitted. */
    int p[2];assert(pipe(p)==0);
    assert(!lj_media_delegate_owkterm(p[1],"https://example.test/x\033\\oops"));
    close(p[1]);assert(read(p[0],out,sizeof out)==0);close(p[0]);
    assert(!lj_media_delegate_owkterm(-1,url));

    lj_media m;lj_media_init(&m);
    assert(m.fps==12);
    assert(lj_media_set_fps(&m,24)&&m.fps==24);
    assert(!lj_media_set_fps(&m,0)&&m.fps==24);
    m.fd=3;assert(!lj_media_set_fps(&m,30)&&m.fps==24);

    puts("media delegate: safe OSC 7771 + pre-open fps PASS");
    return 0;
}
