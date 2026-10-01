#define _XOPEN_SOURCE 700
#include "../liljack_app/c_theme.h"
#include <assert.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void put(const char*p,const char*s){FILE*f=fopen(p,"w");assert(f);fputs(s,f);assert(!fclose(f));}
int main(void){
 assert(setlocale(LC_CTYPE,"C.UTF-8"));char dir[]="/tmp/lj-settings-XXXXXX",path[512],err[256];assert(mkdtemp(dir));snprintf(path,sizeof path,"%s/theme.json",dir);setenv("LILJACK_THEME",path,1);
 lj_theme_reset();assert(lj_theme_int(LJ_THEME_GRAPH_BUCKET_MS)==50);assert(lj_theme_int(LJ_THEME_GRAPH_REPAINT_MS)==100);
 const lj_theme_setting*s=lj_theme_setting_get(LJ_THEME_GRAPH_BUCKET_MS);assert(s&&s->min==25&&s->max==1000&&!strcmp(s->unit,"ms"));
 assert(lj_theme_set_int(LJ_THEME_GRAPH_BUCKET_MS,5000)==1000);assert(lj_theme_int(LJ_THEME_GRAPH_REPAINT_MS)==1000);
 assert(lj_theme_set_int(LJ_THEME_GRAPH_BUCKET_MS,100)==100);assert(lj_theme_set_int(LJ_THEME_GRAPH_REPAINT_MS,200)==200);
 put(path,"{\"version\":1,\"tokens\":{\"bg\":{\"rgb\":\"#123456\"}},\"settings\":{\"graph.bucket_ms\":{\"int\":100,\"min\":25,\"max\":1000,\"unit\":\"ms\"},\"graph.repaint_ms\":{\"int\":200}}}");assert(lj_theme_load(path,err,sizeof err)==1);assert(lj_theme_rgb(LJ_THEME_BG)==0x123456);
 assert(lj_theme_save_user(err,sizeof err)==1);lj_theme_reset();assert(lj_theme_load_default(err,sizeof err)==1);assert(lj_theme_int(LJ_THEME_GRAPH_BUCKET_MS)==100&&lj_theme_int(LJ_THEME_GRAPH_REPAINT_MS)==200);assert(lj_theme_rgb(LJ_THEME_BG)==0x123456);
 const char*bad[]={"{\"int\":1}","{\"int\":100.5}","{\"int\":9223372036854775807}","{\"int\":100,\"unit\":\"s\"}","{\"int\":100,\"max\":999999}","{\"int\":100,\"oops\":2}"};
 for(unsigned i=0;i<sizeof bad/sizeof*bad;i++){char json[1024];snprintf(json,sizeof json,"{\"version\":1,\"tokens\":{\"bg\":{\"rgb\":\"#abcdef\"}},\"settings\":{\"graph.bucket_ms\":%s}}",bad[i]);put(path,json);assert(lj_theme_load(path,err,sizeof err)==-1);assert(lj_theme_int(LJ_THEME_GRAPH_BUCKET_MS)==100&&lj_theme_rgb(LJ_THEME_BG)==0x123456);}
 put(path,"{\"version\":1,\"tokens\":{}}");assert(lj_theme_load(path,err,sizeof err)==1);assert(lj_theme_int(LJ_THEME_GRAPH_BUCKET_MS)==50);
 assert(lj_theme_set_int((lj_theme_setting_id)-1,5)==-1);
 unlink(path);rmdir(dir);puts("theme-settings: defaults, clamp, numeric+colour roundtrip, invalid rollback, legacy themes PASS");return 0;
}
