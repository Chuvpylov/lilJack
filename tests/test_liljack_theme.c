#define _XOPEN_SOURCE 700
#include "../liljack_app/c_theme.h"
#include <assert.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static char path[256],error[256];
static void write_theme(const char*s){FILE*f=fopen(path,"w");assert(f);assert(fputs(s,f)>=0);assert(!fclose(f));}
int main(int argc,char**argv){
    assert(setlocale(LC_CTYPE,"C.UTF-8"));lj_theme_reset();
    if(argc==3&&!strcmp(argv[1],"--defaults"))return lj_theme_write_defaults(argv[2])!=0;
    char dir[]="/tmp/liljack-theme-test-XXXXXX";assert(mkdtemp(dir));snprintf(path,sizeof path,"%s/theme.json",dir);
    uint32_t before=lj_theme_rgb(LJ_THEME_BG);assert(lj_theme_load(path,error,sizeof error)==0);
    assert(lj_theme_write_defaults(path)==0);assert(lj_theme_load(path,error,sizeof error)==1);
    assert(lj_theme_rgb(LJ_THEME_BG)==before);assert(lj_theme_find("not_a_token")==-1);
    for(int i=0;i<LJ_THEME_COUNT;i++){const lj_theme_token*t=lj_theme_get(i);assert(t&&t->ansi16<16&&t->ansi256<256);assert(lj_theme_find(t->name)==i);}
    write_theme("{\"version\":1,\"tokens\":{\"bg\":{\"rgb\":\"#ff0000\"},\"icon_claude\":{\"glyph\":\"中\"}}}");
    assert(lj_theme_load(path,error,sizeof error)==1);assert(lj_theme_rgb(LJ_THEME_BG)==0xff0000);
    assert(lj_theme_get(LJ_THEME_BG)->ansi16==9&&lj_theme_get(LJ_THEME_BG)->ansi256==9);
    assert(!strcmp(lj_theme_glyph(LJ_THEME_ICON_CLAUDE),"中"));
    const char*bad[]={
        "{\"version\":1,\"tokens\":{\"bg\":{\"rgb\":\"#00ff00\"},\"unknown\":{}}}",
        "{\"version\":1,\"tokens\":{\"bg\":{\"rgb\":\"#fffffg\"}}}",
        "{\"version\":1,\"tokens\":{\"bg\":{\"rgb\":123}}}",
        "{\"version\":1,\"tokens\":{\"icon_claude\":{\"glyph\":\"x\"}}}",
        "{\"version\":1,\"tokens\":{\"icon_claude\":{\"glyph\":\"\\u001b[2J\"}}}",
        "{\"version\":1,\"tokens\":{\"bg\":{\"rgb\":\"#ffffff\\u0000\"}}}",
        "{\"version\":2,\"tokens\":{}}", "{}", "[]", "{", "{\"version\":1,\"tokens\":{}} junk"
    };
    for(unsigned i=0;i<sizeof bad/sizeof bad[0];i++){
        write_theme(bad[i]);assert(lj_theme_load(path,error,sizeof error)==-1);assert(*error);
        assert(lj_theme_rgb(LJ_THEME_BG)==0xff0000);assert(!strcmp(lj_theme_glyph(LJ_THEME_ICON_CLAUDE),"中"));
    }
    FILE*f=fopen(path,"w");assert(f);for(int i=0;i<65537;i++)fputc(' ',f);fclose(f);
    assert(lj_theme_load(path,error,sizeof error)==-1);assert(lj_theme_rgb(LJ_THEME_BG)==0xff0000);
    write_theme("{\"version\":1,\"tokens\":{\"bg\":{\"rgb\":\"#00ff00\"}}}");
    assert(!setenv("LILJACK_THEME",path,1));assert(lj_theme_load_default(error,sizeof error)==1);
    assert(lj_theme_rgb(LJ_THEME_BG)==0x00ff00);assert(!strcmp(lj_theme_glyph(LJ_THEME_ICON_CLAUDE),"🧠"));
    unsetenv("LILJACK_THEME");unlink(path);rmdir(dir);lj_theme_reset();assert(lj_theme_rgb(LJ_THEME_BG)==before);
    puts("Theme: default round-trip, mapping, overrides, atomic rejection, glyph width/controls, size limit and explicit path PASS");return 0;
}
