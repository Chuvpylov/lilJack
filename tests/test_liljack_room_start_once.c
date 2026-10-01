/* room-start-once — the operator 2026-09-16: "chat room creates 2 times for some reason,
 * maybe I'm clicking 2 times". The workspace DB confirms it: "lilJack's debug" and
 * "ring dashboard rewiew" were each started twice (a removed twin with its own
 * sessions and duplicate todo rows). Cause: CREATE pressed again (or Enter + a
 * click) before room_start answers is queued by request() and sent after the reply.
 * Production controller, pointer and key events only. Asserts:
 *   S1 CREATE, CREATE, Enter while the first room_start is outstanding writes ONE
 *      room_start to the helper and queues none;
 *   S2 same while the helper is busy with another request: at most one room_start
 *      is queued (none written);
 *   S3 an error reply re-arms CREATE, so a failed start can be retried;
 *   S4 the dialog says it is creating (CREATE shows "CREATING…");
 *   S5 an error reply to an UNRELATED request does not re-arm CREATE (codex, 2103).
 * Fail-before: S1 wrote 2 room_start lines (the second as the queued pending). */
#define main liljack_application_main
#include "../liljack_app/main.c"
#undef main
#include "../liljack_app/c_ansi.c"
#include <assert.h>
#include <fcntl.h>
static int checks,failed;
#define CHECK(c,...) do{checks++;if(!(c)){failed++;fprintf(stderr,"FAIL %s:%d ",__FILE__,__LINE__);fprintf(stderr,__VA_ARGS__);fputc('\n',stderr);}}while(0)
static void pointer(App *a,Uint32 type,int x,int y){SDL_Event e={0};e.type=type;if(type==SDL_MOUSEMOTION){e.motion.x=x;e.motion.y=y;}else{e.button.button=SDL_BUTTON_LEFT;e.button.x=x;e.button.y=y;}event(a,&e);}
static int click(App *a,int kind){render(a);for(int i=0;i<a->nhits;i++)if(a->hits[i].kind==kind){lj_rect r=a->hits[i].r;int x=r.x+r.w/2,y=r.y+r.h/2;pointer(a,SDL_MOUSEBUTTONDOWN,x,y);pointer(a,SDL_MOUSEBUTTONUP,x,y);return 1;}return 0;}
static void press(App *a,SDL_Keycode k){SDL_Event e={0};e.type=SDL_KEYDOWN;e.key.keysym.sym=k;event(a,&e);}
static int count(const char *hay,const char *needle){int n=0;for(const char *p=hay;(p=strstr(p,needle));p+=strlen(needle))n++;return n;}
static int drain(int fd,char *buf,size_t cap){size_t used=0;ssize_t n;while(used+1<cap&&(n=read(fd,buf+used,cap-1-used))>0)used+=(size_t)n;buf[used]=0;return (int)used;}
static App *make(int *to_helper,int *from_helper_w){
    App *a=calloc(1,sizeof *a);assert(a);a->w=1280;a->h=720;a->ansi=a->running=1;a->split=-1;a->mousex=a->mousey=-1;
    for(int i=0;i<COUNT;i++)a->sessions[i].fd=-1;
    lj_dock_init(&a->dock);lj_media_init(&a->media);lj_popup_init(&a->popup);demo(a);a->demo=0;a->toast[0]=0;
    int in[2],out[2];assert(pipe(in)==0&&pipe(out)==0);
    fcntl(in[0],F_SETFL,O_NONBLOCK);fcntl(out[0],F_SETFL,O_NONBLOCK);
    a->backend_in=in[1];*to_helper=in[0];a->backend_out=out[0];*from_helper_w=out[1];
    a->response=malloc(RESPONSE_CAP+1);assert(a->response);   /* start_backend() owns this in the app */
    copy(a->project,sizeof a->project,".");
    room_dialog_open(a);copy(a->room_name,sizeof a->room_name,"Once only");
    return a;
}
int main(void){
    setlocale(LC_CTYPE,"C.UTF-8");assert(!lj_render_init(1280,720));
    char buf[65536];
    /* S1 + S4 */
    {int rd,wr;App *a=make(&rd,&wr);
     click(a,H_ROOM_CREATE);click(a,H_ROOM_CREATE);press(a,SDLK_RETURN);
     drain(rd,buf,sizeof buf);int written=count(buf,"\"room_start\""),queued=a->pending?count(a->pending,"\"room_start\""):0;
     CHECK(written==1&&queued==0,"S1 wrote %d room_start, queued %d (want 1, 0)",written,queued);
     render(a);int creating=0;for(int y=0;y<state.rows;y++){char row[512];size_t n=0;for(int x=0;x<state.cols&&n<500;x++){uint32_t cp=state.canvas[y*state.cols+x].cp;row[n++]=cp&&cp<128?(char)cp:' ';}row[n]=0;if(strstr(row,"CREATING"))creating=1;}
     CHECK(creating,"S4 the dialog does not say CREATING while room_start is outstanding");
     /* S3: an error reply re-arms */
     const char *reply="{\"ok\":false,\"error\":\"folder vanished\"}\n";assert(write(wr,reply,strlen(reply))==(ssize_t)strlen(reply));
     backend_poll(a);click(a,H_ROOM_CREATE);drain(rd,buf,sizeof buf);
     CHECK(count(buf,"\"room_start\"")==1,"S3 after an error reply CREATE sent %d room_start (want 1)",count(buf,"\"room_start\""));
     close(rd);close(wr);free(a->pending);free(a->response);free(a);}
    /* S2 */
    {int rd,wr;App *a=make(&rd,&wr);a->busy=1;
     click(a,H_ROOM_CREATE);click(a,H_ROOM_CREATE);press(a,SDLK_RETURN);
     drain(rd,buf,sizeof buf);int written=count(buf,"\"room_start\""),queued=a->pending?count(a->pending,"\"room_start\""):0;
     CHECK(written==0&&queued==1,"S2 wrote %d, queued %d (want 0, 1)",written,queued);
     const char *reply="{\"ok\":true,\"data\":{}}\n";assert(write(wr,reply,strlen(reply))==(ssize_t)strlen(reply));
     /* the helper answers whatever is outstanding until the queued start has gone out */
     int sent=0;for(int round=0;round<6;round++){backend_poll(a);drain(rd,buf,sizeof buf);sent+=count(buf,"\"room_start\"");
         if(!a->pending)break;assert(write(wr,reply,strlen(reply))==(ssize_t)strlen(reply));}
     strcpy(buf,sent==1?"\"room_start\"":"");
     CHECK(sent==1,"S2 the queued start was sent %d times after the busy reply (want 1)",sent);
     click(a,H_ROOM_CREATE);drain(rd,buf,sizeof buf);
     CHECK(count(buf,"\"room_start\"")==0&&!(a->pending&&strstr(a->pending,"room_start")),"S2 a click while the queued start is outstanding sent another");
     close(rd);close(wr);free(a->pending);free(a->response);free(a);}
    /* S5 (codex review, room 2103): an ERROR for an unrelated in-flight request must not
     * clear the guard while the start is still queued behind it. */
    {int rd,wr;App *a=make(&rd,&wr);a->busy=1;
     click(a,H_ROOM_CREATE);
     const char *err="{\"ok\":false,\"error\":\"earlier unrelated request failed\"}\n";assert(write(wr,err,strlen(err))==(ssize_t)strlen(err));
     backend_poll(a);drain(rd,buf,sizeof buf);int first=count(buf,"\"room_start\"");
     click(a,H_ROOM_CREATE);press(a,SDLK_RETURN);drain(rd,buf,sizeof buf);
     int dup=count(buf,"\"room_start\"")+(a->pending&&strstr(a->pending,"room_start")?1:0);
     CHECK(first==1&&dup==0,"S5 unrelated error: first start sent %d, then %d more sent or queued (want 1, 0)",first,dup);
     close(rd);close(wr);free(a->pending);free(a->response);free(a);}
    lj_render_close();
    fprintf(stderr,"%s room-start-once: %d checks, %d failed (one room_start per CREATE burst, queued or not; error re-arms; CREATING shown)\n",failed?"FAIL":"PASS",checks,failed);
    return failed?1:0;
}
