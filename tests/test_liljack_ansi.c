#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include "../liljack_app/c_ansi.h"
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
static int master=-1,slave=-1,saved_in=-1,saved_out=-1;
#define CHECK(c) do {if(!(c)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
static void cleanup(void){lj_ansi_close();if(saved_in>=0){dup2(saved_in,0);close(saved_in);saved_in=-1;}if(saved_out>=0){dup2(saved_out,1);close(saved_out);saved_out=-1;}if(master>=0){close(master);master=-1;}if(slave>=0){close(slave);slave=-1;}}
static void pause_ms(int ms){struct timespec t={ms/1000,(ms%1000)*1000000};nanosleep(&t,NULL);}
static SDL_Event next(void){SDL_Event e={0};for(int i=0;i<100;i++){if(lj_ansi_poll(&e))return e;pause_ms(2);}CHECK(0);return e;}
static void inject(const char *s,size_t n){size_t sent=0;while(sent<n){ssize_t r=write(master,s+sent,n-sent);if(r<0&&errno==EINTR)continue;CHECK(r>0);sent+=(size_t)r;}}
static void input(const char *s){inject(s,strlen(s));}
static size_t drain(char *out,size_t cap){size_t used=0;for(;;){ssize_t n=read(master,out+used,cap-used-1);if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK))break;CHECK(n>=0);if(!n)break;used+=(size_t)n;CHECK(used+1<cap);}out[used]=0;return used;}
int main(void){
    setlocale(LC_CTYPE,"");CHECK(atexit(cleanup)==0);
    struct winsize ws={.ws_col=12,.ws_row=6};CHECK(openpty(&master,&slave,NULL,NULL,&ws)==0);CHECK(fcntl(master,F_SETFL,O_NONBLOCK)==0);
    struct termios before,after;CHECK(tcgetattr(slave,&before)==0);int flags=fcntl(slave,F_GETFL);CHECK(flags>=0);
    saved_in=dup(0);saved_out=dup(1);CHECK(saved_in>=0&&saved_out>=0);CHECK(dup2(slave,0)==0);CHECK(dup2(slave,1)==1);
    int w,h;CHECK(lj_ansi_open(&w,&h));CHECK(w==120&&h==120);CHECK(tcgetattr(slave,&after)==0);CHECK(!(after.c_lflag&(ECHO|ICANON|ISIG)));CHECK(after.c_cc[VMIN]==0&&after.c_cc[VTIME]==0);CHECK(fcntl(0,F_GETFL)==flags);
    char output[32768];drain(output,sizeof output);CHECK(strstr(output,"?1049h")&&strstr(output,"?1006h")&&strstr(output,"?2004h"));
    const char *copy_text[]={"a","ab","abc","日","\033",""};
    const char *copy_encoded[]={"YQ==","YWI=","YWJj","5pel","Gw==",""};
    for(int i=0;i<6;i++){
        CHECK(lj_ansi_copy(copy_text[i]));drain(output,sizeof output);
        char expected[64];snprintf(expected,sizeof expected,"\033]52;c;%s\007",copy_encoded[i]);CHECK(!strcmp(output,expected));
    }
    char *large=malloc(65538);CHECK(large);memset(large,'x',65537);large[65537]=0;
    CHECK(!lj_ansi_copy(large));CHECK(!lj_ansi_copy(NULL));CHECK(drain(output,sizeof output)==0);free(large);
    CHECK(lj_ansi_mouse(0));drain(output,sizeof output);CHECK(strstr(output,"?1003l")&&strstr(output,"?1006l"));
    CHECK(lj_ansi_mouse(1));drain(output,sizeof output);CHECK(strstr(output,"?1003h")&&strstr(output,"?1006h"));
    lj_ansi_begin(w,h);lj_ansi_rect(0,0,w,h,0x030b03);lj_ansi_rect(0,0,120,1,0x00ff41);lj_ansi_text(0,20,"AПр 🌿",0x00ff41,120);lj_ansi_glyph(10,60,0x65e5,0x00fff9,2);
    CHECK(lj_ansi_present());drain(output,sizeof output);CHECK(strstr(output,"38;2;0;255;65")&&strstr(output,"Пр")&&strstr(output,"日")&&strstr(output,"─"));
    CHECK(lj_ansi_present());CHECK(drain(output,sizeof output)==0); /* unchanged canvas emits nothing */
    input("\033[A");SDL_Event e=next();CHECK(e.type==SDL_KEYDOWN&&e.key.keysym.sym==SDLK_UP);
    input("\033[1;5D");e=next();CHECK(e.key.keysym.sym==SDLK_LEFT&&(e.key.keysym.mod&KMOD_CTRL));
    input("\033[17~");e=next();CHECK(e.key.keysym.sym==SDLK_F6);
    input("\003");e=next();CHECK(e.key.keysym.sym==SDLK_c&&(e.key.keysym.mod&KMOD_CTRL));
    input("\033x");e=next();CHECK(e.type==SDL_TEXTINPUT&&!strcmp(e.text.text,"x")&&(lj_ansi_modifiers()&KMOD_ALT));
    inject("\xd0",1);CHECK(!lj_ansi_poll(&e));inject("\x9f",1);e=next();CHECK(e.type==SDL_TEXTINPUT&&!strcmp(e.text.text,"П"));
    input("\033[<4;3;2M");e=next();CHECK(e.type==SDL_MOUSEBUTTONDOWN&&e.button.button==SDL_BUTTON_LEFT&&e.button.x==25&&e.button.y==30&&(lj_ansi_modifiers()&KMOD_SHIFT));
    input("\033[<32;4;3M");e=next();CHECK(e.type==SDL_MOUSEMOTION&&(e.motion.state&SDL_BUTTON_LMASK));
    input("\033[<0;4;3m");e=next();CHECK(e.type==SDL_MOUSEBUTTONUP);
    /* ?1016: pixel reports map through the cell size (10x20 here) to lilJack pixels */
    lj_ansi_pixel_mouse(1);input("\033[<0;151;341M");e=next();CHECK(e.type==SDL_MOUSEBUTTONDOWN&&e.button.x==150&&e.button.y==340);
    input("\033[<32;156;350M");e=next();CHECK(e.type==SDL_MOUSEMOTION&&e.motion.x==155&&e.motion.y==349);input("\033[<0;156;350m");e=next();CHECK(e.type==SDL_MOUSEBUTTONUP);lj_ansi_pixel_mouse(0);
    input("\033[<64;5;4M");e=next();CHECK(e.type==SDL_MOUSEMOTION&&e.motion.x==45&&e.motion.y==70);e=next();CHECK(e.type==SDL_MOUSEWHEEL&&e.wheel.y==1);
    /* One owned paste event preserves newline, never a KEYDOWN Enter. */
    input("\033[200~hello\nПривіт\033[20");for(int i=0;i<3;i++)CHECK(!lj_ansi_poll(&e));input("1~");e=next();CHECK(e.type==SDL_USEREVENT&&e.user.code==LJ_ANSI_PASTE_CODE&&!strcmp(e.user.data1,"hello\nПривіт"));free(e.user.data1);
    /* Oversized paste is wholly discarded through its end marker. */
    input("\033[200~");CHECK(!lj_ansi_poll(&e));char block[1024];memset(block,'x',sizeof block);
    for(int i=0;i<65;i++){inject(block,sizeof block);CHECK(!lj_ansi_poll(&e));}
    input("\033[201~");e=next();CHECK(e.type==SDL_USEREVENT&&e.user.code==LJ_ANSI_ERROR_CODE);free(e.user.data1);
    input("z");e=next();CHECK(e.type==SDL_TEXTINPUT&&!strcmp(e.text.text,"z"));
    /* OSC payloads are swallowed even across fragmented input. */
    input("\033]52;c;untrusted\n");CHECK(!lj_ansi_poll(&e));CHECK(!lj_ansi_poll(&e));input("payload\007q");CHECK(!lj_ansi_poll(&e));e=next();CHECK(e.type==SDL_TEXTINPUT&&!strcmp(e.text.text,"q"));
    input("\033[99999999999999999999~r");CHECK(!lj_ansi_poll(&e));e=next();CHECK(e.type==SDL_TEXTINPUT&&!strcmp(e.text.text,"r"));
    input("\033");CHECK(!lj_ansi_poll(&e));pause_ms(50);e=next();CHECK(e.type==SDL_KEYDOWN&&e.key.keysym.sym==SDLK_ESCAPE);
    /* Below the declared minimum the composer is silently lost, so the renderer says so.
     * Reporting, never clamping: the canvas keeps the size the terminal actually has. */
    lj_ansi_begin(LJ_ANSI_MIN_COLS*10,LJ_ANSI_MIN_ROWS*20);CHECK(lj_ansi_present());drain(output,sizeof output);
    CHECK(!strstr(output,"lilJack needs"));
    lj_ansi_begin((LJ_ANSI_MIN_COLS-1)*10,LJ_ANSI_MIN_ROWS*20);CHECK(lj_ansi_present());drain(output,sizeof output);
    CHECK(strstr(output,"terminal 79x28")&&strstr(output,"lilJack needs 80x28")&&strstr(output,"composer hidden"));
    lj_ansi_begin(LJ_ANSI_MIN_COLS*10,(LJ_ANSI_MIN_ROWS-1)*20);CHECK(lj_ansi_present());drain(output,sizeof output);
    CHECK(strstr(output,"terminal 80x27")&&strstr(output,"lilJack needs 80x28"));
    CHECK(lj_ansi_present());CHECK(drain(output,sizeof output)==0); /* notice is stable, not a repaint loop */
    /* Effects are cell-native and off by default: alternate rows get a dimmed
     * BACKGROUND and every glyph survives, which is the whole safety claim.
     * Each frame uses a DIFFERENT canvas size so the diff buffer is reset and the
     * full frame is re-emitted - otherwise unchanged rows are correctly silent and
     * the assertions would be testing the diff, not the effect. */
    lj_ansi_begin(LJ_ANSI_MIN_COLS*10,LJ_ANSI_MIN_ROWS*20);
    lj_ansi_rect(0,0,LJ_ANSI_MIN_COLS*10,LJ_ANSI_MIN_ROWS*20,0x204080);
    lj_ansi_text(0,20,"ROWA",0x00ff41,200);lj_ansi_text(0,40,"ROWB",0x00ff41,200);
    CHECK(lj_ansi_present());drain(output,sizeof output);
    CHECK(strstr(output,"48;2;32;64;128")&&!strstr(output,"48;2;24;48;96"));
    CHECK(strstr(output,"ROWA")&&strstr(output,"ROWB"));
    lj_ansi_effects(1);
    lj_ansi_begin(LJ_ANSI_MIN_COLS*10,(LJ_ANSI_MIN_ROWS+1)*20);
    lj_ansi_rect(0,0,LJ_ANSI_MIN_COLS*10,(LJ_ANSI_MIN_ROWS+1)*20,0x204080);
    lj_ansi_text(0,20,"ROWA",0x00ff41,200);lj_ansi_text(0,40,"ROWB",0x00ff41,200);
    CHECK(lj_ansi_present());drain(output,sizeof output);
    CHECK(strstr(output,"48;2;24;48;96"));               /* odd rows dimmed to 3/4 */
    CHECK(strstr(output,"48;2;32;64;128"));              /* even rows untouched */
    CHECK(strstr(output,"ROWA")&&strstr(output,"ROWB")); /* NO GLYPH LOST */
    lj_ansi_effects(0);
    ws.ws_col=100;ws.ws_row=40;CHECK(ioctl(slave,TIOCSWINSZ,&ws)==0);e=next();CHECK(e.type==SDL_WINDOWEVENT&&e.window.data1==1000&&e.window.data2==800);
    lj_ansi_close();CHECK(tcgetattr(slave,&after)==0);CHECK(before.c_iflag==after.c_iflag&&before.c_oflag==after.c_oflag&&before.c_lflag==after.c_lflag&&before.c_cflag==after.c_cflag);CHECK(fcntl(0,F_GETFL)==flags);drain(output,sizeof output);CHECK(strstr(output,"?1049l")&&strstr(output,"?2004l"));
    /* owkTerm advertises the one terminal where we know DECSET 1016 works.
     * A fresh open must request it immediately and decode sub-cell motion. */
    unsetenv("TMUX");   /* the suite often runs inside a lilJack tmux tile */
    CHECK(setenv("OWKTERM","1",1)==0);CHECK(lj_ansi_open(&w,&h));drain(output,sizeof output);CHECK(strstr(output,"?1016h"));
    input("\033[<0;156;350M");e=next();CHECK(e.type==SDL_MOUSEBUTTONDOWN&&e.button.x==155&&e.button.y==349);
    /* Leaving must switch pixel reports OFF again, or the next mouse app in
     * that owkTerm (vim, htop, a shell) gets pixels where it expects cells. */
    lj_ansi_close();drain(output,sizeof output);CHECK(strstr(output,"?1016l"));
    /* tmux inherits OWKTERM=1 from owkTerm but answers mouse in CELLS itself:
     * no ?1016 there, and reports keep their cell meaning. */
    CHECK(setenv("TMUX","/tmp/tmux-test,1,0",1)==0);CHECK(lj_ansi_open(&w,&h));drain(output,sizeof output);CHECK(!strstr(output,"?1016h"));
    input("\033[<0;15;35M");e=next();CHECK(e.type==SDL_MOUSEBUTTONDOWN&&e.button.x==145&&e.button.y==690);
    CHECK(lj_ansi_mouse(1));drain(output,sizeof output);CHECK(!strstr(output,"?1016h"));   /* F8 toggle path too */
    lj_ansi_close();CHECK(unsetenv("TMUX")==0);CHECK(unsetenv("OWKTERM")==0);drain(output,sizeof output);
    cleanup();puts("C ANSI: raw terminal restoration, UTF-8 truecolor diff canvas, keys/mouse/resize, paste isolation and overflow rejection passed");return 0;
}
