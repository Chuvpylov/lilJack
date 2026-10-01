/* Reuse the owned-terminal query/drain helpers; no live pane input. */
#define main ansi_image_test_main
#include "test_liljack_ansi_image.c"
#undef main
int main(void){
    int master=posix_openpt(O_RDWR|O_NOCTTY);CHECK(master>=0);
    CHECK(!grantpt(master)&&!unlockpt(master));
    char *name=ptsname(master);CHECK(name);
    FILE *log=tmpfile();CHECK(log);pid_t pid=fork();CHECK(pid>=0);
    if(!pid){
        int slave=open(name,O_RDWR|O_NOCTTY);CHECK(slave>=0);
        close(master);dup2(slave,0);dup2(slave,1);dup2(fileno(log),2);
        if(slave>2)close(slave);
        setenv("LILJACK_TRACE_OVERLAYS","1",1);unsetenv("LILJACK_ANSI_BORDER_ROWS");
        unsetenv("LJ_SIXEL_OFF");int w,h;CHECK(lj_ansi_open(&w,&h));
        for(int frame=0;frame<2;frame++){
            lj_ansi_begin(400,200);lj_ansi_rect(0,0,400,200,0x101010);
            lj_ansi_trace_lead(frame?"s-current-lead":"s-original-lead",20,40,150,100);
            CHECK(lj_ansi_border(20,40,150,100,0xffcc00,0x00ffff,0));
            lj_ansi_separator(200,40,10,100,0xffcc00,1);
            CHECK(lj_ansi_present());CHECK(lj_ansi_border_tick(20));
        }
        lj_ansi_close();_exit(0);
    }
    char query[256];CHECK(read_query(master,query,sizeof query,'c',400)>0);
    const char *da="\033[?6;4;2c";CHECK(write(master,da,strlen(da))==(ssize_t)strlen(da));
    CHECK(read_query(master,query,sizeof query,'t',400)>0);
    const char *pixels="\033[6;16;8t";CHECK(write(master,pixels,strlen(pixels))==(ssize_t)strlen(pixels));
    static char wire[1<<18];int n=drain(master,wire,sizeof wire,900);
    int status;CHECK(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&!WEXITSTATUS(status));close(master);
    CHECK(count_dcs(wire,n)==9); /* initial 4 edges+separator, one tick; repeat is silent */
    rewind(log);char trace[8192];size_t size=fread(trace,1,sizeof(trace)-1,log);trace[size]=0;fclose(log);
    CHECK(strstr(trace,"sixel=1 cell=8x16 border_rows=0 queued=2 frames=1 separators=1 prepared=2 emitted=2"));
    CHECK(strstr(trace,"lead=s-original-lead")&&strstr(trace,"lead=s-current-lead"));
    CHECK(strstr(trace,"tile=s-current-lead rect=20,40,150,100"));
    CHECK(strstr(trace,"kind=tick")&&strstr(trace,"retired=2"));
    int lines=0;for(size_t i=0;i<size;i++)lines+=trace[i]=='\n';CHECK(lines==3);
    CHECK(!strstr(wire,"liljack-overlays"));
    puts(trace);puts("Overlay trace PTY: capabilities, frame/separator emission, ticks, retirement and changed lead identity PASS");
    return 0;
}
