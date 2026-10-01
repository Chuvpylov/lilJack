#define _XOPEN_SOURCE 700
#include "../liljack_app/c_ansi.c"
#include "../liljack_app/c_metrics.c"
#include <assert.h>
int main(void){
 lj_theme_reset();state.opened=1;state.sixel=1;state.cellw=17;state.cellh=39;
 for(int hot=0;hot<2;hot++){
  int id=hot?LJ_THEME_DIVIDER_HOT_PX:LJ_THEME_DIVIDER_IDLE_PX;
  lj_theme_set_int(id,hot?5:2);lj_ansi_begin(200,200);
  lj_ansi_separator_hot(20,20,10,160,0xff0000,1,hot);
  assert(state.border_count==1);pixel_border*b=&state.borders[0];assert(b->thickness==(hot?5:2));assert(prepare_separator(b));assert(b->bytes>0);
 }
 memset(&G,0,sizeof G);G.display_count=1;G.display_now_ms=1000;G.display[LJ_METRIC_MEM][0]=0.25f;
 lj_theme_set_int(LJ_THEME_METRICS_LABEL_HOLD_MS,1000);lj_metrics_set_label_hold_ms(lj_theme_int(LJ_THEME_METRICS_LABEL_HOLD_MS));
 char first[32];snprintf(first,sizeof first,"%s",lj_metrics_display_label(LJ_METRIC_MEM));
 G.display_now_ms=1600;G.display[LJ_METRIC_MEM][0]=0.75f;assert(!strcmp(first,lj_metrics_display_label(LJ_METRIC_MEM)));
 lj_metrics_set_label_hold_ms(100);assert(strcmp(first,lj_metrics_display_label(LJ_METRIC_MEM)));assert(lj_metrics_label_ms()==100);
 G.inited=1;G.fd_stat=G.fd_meminfo=-1;G.last_poll_s=monotonic()-0.100;double before=G.last_poll_s;
 lj_theme_set_int(LJ_THEME_METRICS_POLL_MS,1000);lj_metrics_set_interval_ms(lj_theme_int(LJ_THEME_METRICS_POLL_MS));lj_metrics_poll();assert(G.last_poll_s==before);
 lj_metrics_set_interval_ms(25);lj_metrics_poll();assert(G.last_poll_s>before);
 lj_theme_reset();puts("theme-consumers: live divider widths2/5, label hold1000->100, poll1000->25 PASS");return 0;
}
