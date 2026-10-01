#define _XOPEN_SOURCE 700
#include "../liljack_app/c_theme.h"
#include "../liljack_app/c_render.h"
#include <assert.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(void){
 assert(setlocale(LC_CTYPE,"C.UTF-8"));lj_theme_reset();assert(LJ_THEME_SETTING_COUNT==8);
 char dir[]="/tmp/lj-full-theme-XXXXXX",path[512],err[256];assert(mkdtemp(dir));snprintf(path,sizeof path,"%s/theme.json",dir);setenv("LILJACK_THEME",path,1);
 int values[LJ_THEME_SETTING_COUNT];
 for(int i=0;i<LJ_THEME_SETTING_COUNT;i++){const lj_theme_setting*s=lj_theme_setting_get(i);assert(s&&s->step>0);values[i]=lj_theme_set_int(i,s->max);assert(values[i]==s->max);}
 for(int i=0;i<LJ_THEME_COUNT;i++){uint32_t value=(0x102030u+(unsigned)i*137u)&0xffffffu;assert(lj_theme_set_rgb(i,value)==value);assert(lj_theme_rgb(i)==value);assert(lj_theme_get(i)->ansi16<16&&lj_theme_get(i)->ansi256<256);}
 assert(lj_theme_save_user(err,sizeof err)==1);lj_theme_reset();assert(lj_theme_load_default(err,sizeof err)==1);
 for(int i=0;i<LJ_THEME_SETTING_COUNT;i++)assert(lj_theme_int(i)==values[i]);
 for(int i=0;i<LJ_THEME_COUNT;i++)assert(lj_theme_rgb(i)==((0x102030u+(unsigned)i*137u)&0xffffffu));
 assert(!lj_render_init(20,20));lj_render_rect(0,0,20,20,lj_theme_rgb(LJ_THEME_BG));assert((lj_render_pixels()[0]&0xffffff)==lj_theme_rgb(LJ_THEME_BG));
 lj_theme_set_rgb(LJ_THEME_BG,0xabcdef);lj_render_rect(0,0,20,20,lj_theme_rgb(LJ_THEME_BG));assert((lj_render_pixels()[0]&0xffffff)==0xabcdef);lj_render_close();
 unlink(path);rmdir(dir);lj_theme_reset();puts("full-theme: all 8 numeric keys + all RGB tokens persisted; edited token changes framebuffer PASS");return 0;
}
