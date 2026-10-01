/* Regression over the actual owkTerm committed pixel sink. */
#define main owkterm_application_main
#include "../../owkTerm/owkterm_app.c"
#undef main
#include "../liljack_app/c_sixel.h"
#include <assert.h>
static Tiles store;
static void feed(void*v,const char*s){lj_vt_feed(v,(const unsigned char*)s,strlen(s));}
static void *fresh(void){
 free(store.px);free(store.freelist);free(store.mark);memset(&store,0,sizeof store);
 store.cap=TILES;store.fresh=1;store.tp=CELLW*CELLH;
 store.px=calloc(store.cap*store.tp,sizeof(uint32_t));store.freelist=calloc(store.cap,sizeof(uint32_t));store.mark=calloc(store.cap,1);
 void*v=lj_vt_new(20,10);assert(v);store.vt=v;
 lj_vt_pixel_sink sink={&store,CELLW,CELLH,tile_new,tile_plot,tile_reset,image_end};lj_vt_set_pixel_sink(v,&sink);return v;
}
static unsigned px(void*v,int x,int y){const lj_cell*k=lj_vt_row(v,y/CELLH)+x/CELLW;return k->marks[1]?store.px[k->marks[1]*store.tp+y%CELLH*CELLW+x%CELLW]&0xffffff:k->bg&0xffffff;}
static int matches(void*v,const uint32_t*expected){for(int y=0;y<20;y++)for(int x=0;x<80;x++)if(px(v,x,y)!=(expected[y*80+x]&0xffffff))return 0;return 1;}
int main(int argc,char**argv){
 uint32_t before[1600],after[1600];for(int y=0;y<20;y++)for(int x=0;x<80;x++){before[y*80+x]=y>=20-(x%19+1)?0xffff0000:0xff000000;after[y*80+x]=y>=20-((79-x)%19+1)?0xff00ff00:0xff000000;}
 char oldwire[65536],newwire[65536];int oldn=lj_sixel_encode(before,80,20,0,0,80,20,oldwire,sizeof oldwire),newn=lj_sixel_encode(after,80,20,0,0,80,20,newwire,sizeof newwire);assert(oldn>0&&newn>0);
 int cuts=0,partial=0,split=-1,maxgap=-1;for(int cut=1;cut<newn;cut++){
  void*v=fresh();lj_vt_feed(v,(unsigned char*)oldwire,oldn);assert(matches(v,before));feed(v,"\033[H");lj_vt_feed(v,(unsigned char*)newwire,cut);
  if(!matches(v,before)&&!matches(v,after)){partial++;int gap=0;for(int x=0;x<80;x++)gap+=px(v,x,19)==0;if(gap>maxgap){maxgap=gap;split=cut;}}
  assert(matches(v,before));cuts++;lj_vt_feed(v,(unsigned char*)newwire+cut,newn-cut);assert(matches(v,after));lj_vt_free(v);
 }
 printf("production sixel bytes old=%d new=%d; cut points=%d partial-visible=%d; final decoded errors=0; selected cut=%d missing-baseline-columns=%d\n",oldn,newn,cuts,partial,split,maxgap);
 if(argc==2){char p[1024];snprintf(p,sizeof p,"%s/old.six",argv[1]);FILE*f=fopen(p,"wb");assert(f);fwrite(oldwire,1,oldn,f);fclose(f);snprintf(p,sizeof p,"%s/new.six",argv[1]);f=fopen(p,"wb");assert(f);fwrite(newwire,1,newn,f);fclose(f);snprintf(p,sizeof p,"%s/split.txt",argv[1]);f=fopen(p,"w");assert(f);fprintf(f,"%d\n",split);fclose(f);}
 return partial || maxgap>0 ? 1 : 0;
}
