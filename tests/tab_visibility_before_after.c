#define LJ_NATIVE_HELPERS_ONLY
#include "test_liljack_native.c"
#undef LJ_NATIVE_HELPERS_ONLY
#include "../docs/reports/2026-09-13-tabs-landed/agentbar-before.inc"
#include <assert.h>
int main(void){setlocale(LC_ALL,"");assert(!SDL_Init(SDL_INIT_VIDEO));assert(!lj_render_init(800,560));
 App*a=calloc(1,sizeof*a);assert(a);test_app=a;a->w=800;a->h=560;a->demo=1;a->split=-1;a->backend_in=a->backend_out=-1;a->mousex=a->mousey=-1;
 for(int i=0;i<COUNT;i++)a->sessions[i].fd=-1;lj_dock_init(&a->dock);lj_media_init(&a->media);lj_popup_init(&a->popup);demo(a);
 for(int pass=0;pass<2;pass++){a->nhits=0;if(pass)draw_agentbar(a,(lj_rect){0,40,800,20});else draw_agentbar_before(a,(lj_rect){0,40,800,20});int n=0;for(int i=0;i<a->nhits;i++)n+=a->hits[i].kind==H_SESSION;
 printf("%s 800px production agentbar: visible=%d required=3 %s\n",pass?"AFTER":"BEFORE",n,n==3?"PASS":"FAIL");assert(pass?n==3:n<3);}
 return 0;}
