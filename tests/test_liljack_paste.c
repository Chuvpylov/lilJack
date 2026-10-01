/* Paste into a tile (the operator 2026-09-13: "I can't paste in tile terminals anything").
 * A host paste (bracketed, from owkTerm/WezTerm) must reach the focused tile even
 * when the app inside never enabled bracketed paste: sent raw then, wrapped in
 * ESC[200~ … ESC[201~ when the app asked for brackets. Never dropped. */
#define LJ_NATIVE_HELPERS_ONLY
#include "test_liljack_native.c"
#undef LJ_NATIVE_HELPERS_ONLY
#define mouse_event ansi_parser_mouse_event
#include "../liljack_app/c_ansi.c"
#undef mouse_event
#include <assert.h>
#include <poll.h>
static ssize_t read_timeout(int fd,char *buf,size_t cap){struct pollfd p={fd,POLLIN,0};if(poll(&p,1,300)<=0)return 0;return read(fd,buf,cap);}
int main(void){
 setlocale(LC_ALL,"");assert(!SDL_Init(SDL_INIT_VIDEO));
 App*a=calloc(1,sizeof*a);assert(a);test_app=a;a->w=1280;a->h=720;a->demo=1;a->ansi=1;a->split=-1;a->mousex=a->mousey=-1;a->backend_in=a->backend_out=-1;for(int i=0;i<COUNT;i++)a->sessions[i].fd=-1;
 lj_dock_init(&a->dock);lj_media_init(&a->media);lj_popup_init(&a->popup);demo(a);assert(!lj_render_init(a->w,a->h));state.opened=1;state.sixel=1;state.cellw=10;state.cellh=20;render(a);
 Session *s=NULL;for(int i=0;i<COUNT&&!s;i++)if(a->sessions[i].vt&&a->sessions[i].id[0])s=&a->sessions[i];assert(s);
 /* leave demo replay: a live, connected tile whose pty is our pipe */
 int fds[2];assert(pipe(fds)==0);a->demo=0;s->connected=1;s->native=0;s->fd=fds[1];copy(a->focus,sizeof(a->focus),s->id);
 if(!s->queue)s->queue=calloc(INPUT_CAP,1);s->queued=0;   /* the pty input queue the main loop drains */
 int bad=0;SDL_Event e;
 /* 1. the app inside has NOT enabled bracketed paste (DECSET 2004 off): the host paste must arrive raw */
 lj_vt_feed(s->vt,(const unsigned char*)"\033[?2004l",8);assert(!lj_vt_state(s->vt,6));
 memset(&e,0,sizeof e);e.type=SDL_USEREVENT;e.user.code=LJ_ANSI_PASTE_CODE;e.user.data1=strdup("echo hello\n");event(a,&e);
 size_t n=s->queued;
 printf("paste: app without brackets: queued %zu bytes %s\n",n,n>0?"(raw)":"(DROPPED)");
 if(n!=11||memcmp(s->queue,"echo hello\n",11))bad++;s->queued=0;
 /* 2. the app enabled bracketed paste: wrapped */
 lj_vt_feed(s->vt,(const unsigned char*)"\033[?2004h",8);assert(lj_vt_state(s->vt,6));
 memset(&e,0,sizeof e);e.type=SDL_USEREVENT;e.user.code=LJ_ANSI_PASTE_CODE;e.user.data1=strdup("x");event(a,&e);
 n=s->queued;int wrapped=n==13&&!memcmp(s->queue,"\033[200~x\033[201~",13);
 printf("paste: app with brackets: queued %zu bytes wrapped=%d\n",n,wrapped);if(!wrapped)bad++;
 printf("paste: %s\n",bad?"FAIL":"PASS");return bad?1:0;
}
