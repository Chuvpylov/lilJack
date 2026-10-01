#include "../liljack_app/c_dock.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static lj_dock_tile *tile(lj_dock *d, const char *id) {
    int i;
    for (i = 0; i < d->tile_count; i++) if (!strcmp(d->tiles[i].id, id)) return &d->tiles[i];
    assert(0); return NULL;
}
static void bad_file(lj_dock *d, const char *path, const char *data) {
    lj_dock before = *d;
    FILE *f = fopen(path, "w");
    assert(f); assert(fputs(data, f) >= 0); assert(!fclose(f));
    assert(!lj_dock_load(d, path)); assert(!memcmp(d, &before, sizeof before));
}
static lj_rect transpose(lj_rect r,int flip){return flip?(lj_rect){r.y,r.x,r.h,r.w}:r;}
static void topology(int flip,int stagger,int which){
    lj_dock d;lj_dock_init(&d);
    assert(lj_dock_drop(&d,"left","room",LJ_DOCK_LEFT));
    assert(lj_dock_drop(&d,"bottom-left","left",LJ_DOCK_BOTTOM));
    assert(lj_dock_drop(&d,"top-right","room",LJ_DOCK_TOP));
    int root=d.root,left=d.nodes[root].a,right=d.nodes[root].b;
    if(stagger)d.nodes[left].ratio=.625;
    if(flip)for(int i=0;i<LJ_DOCK_MAX_NODES;i++)if(d.nodes[i].used&&d.nodes[i].axis)d.nodes[i].axis=3-d.nodes[i].axis;
    lj_rect area=transpose((lj_rect){0,60,1280,620},flip);
    assert(lj_dock_layout(&d,area));
    lj_dock_tile before[4];memcpy(before,d.tiles,sizeof before);
    int node=which==0?root:which==1?left:right;
    lj_dock_divider v=d.dividers[which];assert(v.node==node);
    lj_dock_drag drag;int x=v.logical.x+v.logical.w/2,y=v.logical.y+v.logical.h/2;
    assert(lj_dock_drag_begin(&d,node,x,y,&drag));
    assert(drag.count==(!stagger&&which?2:1));
    int dx=drag.axis==1?30:0,dy=drag.axis==2?30:0;
    assert(lj_dock_drag_move(&d,&drag,x,y));assert(lj_dock_layout(&d,area));
    for(int i=0;i<4;i++)assert(!memcmp(&before[i].rect,&tile(&d,before[i].id)->rect,sizeof(lj_rect)));
    assert(lj_dock_drag_move(&d,&drag,x+dx,y+dy));assert(lj_dock_layout(&d,area));
    for(int i=0;i<4;i++){
        lj_rect expected=transpose(before[i].rect,flip);const char *id=before[i].id;
        int onleft=!strcmp(id,"left")||!strcmp(id,"bottom-left");
        int ontop=!strcmp(id,"left")||!strcmp(id,"top-right");
        if(which==0){if(onleft)expected.w+=30;else{expected.x+=30;expected.w-=30;}}
        else if(!stagger||(which==1&&onleft)||(which==2&&!onleft)){
            if(ontop)expected.h+=30;else{expected.y+=30;expected.h-=30;}
        }
        expected=transpose(expected,flip);
        assert(!memcmp(&expected,&tile(&d,id)->rect,sizeof expected));
    }
    if(!stagger&&which){
        assert(lj_dock_drag_move(&d,&drag,INT_MAX,INT_MAX));assert(lj_dock_layout(&d,area));
        int a=flip?d.dividers[1].rect.x:d.dividers[1].rect.y;
        int b=flip?d.dividers[2].rect.x:d.dividers[2].rect.y;assert(a==b);
    }
    if(stagger&&which==1){
        /* A gesture does not capture a second divider when passing its line. */
        int delta=-76; /* left starts y442; right y366 */
        assert(lj_dock_drag_move(&d,&drag,x+(flip?delta:0),y+(flip?0:delta)));
        assert(lj_dock_layout(&d,area));
        assert(lj_dock_drag_move(&d,&drag,x+(flip?delta-20:0),y+(flip?0:delta-20)));
        assert(lj_dock_layout(&d,area));
        for(int i=0;i<4;i++)if(!strcmp(before[i].id,"top-right")||!strcmp(before[i].id,"room"))
            assert(!memcmp(&before[i].rect,&tile(&d,before[i].id)->rect,sizeof(lj_rect)));
    }
}
static void unequal_group_bounds(void){
    lj_dock d;lj_dock_init(&d);
    assert(lj_dock_drop(&d,"left","room",LJ_DOCK_LEFT));
    assert(lj_dock_drop(&d,"bottom-left","left",LJ_DOCK_BOTTOM));
    assert(lj_dock_drop(&d,"top-right","room",LJ_DOCK_TOP));
    int right=d.nodes[d.root].b;d.nodes[right].ratio=.23;
    assert(lj_dock_drop(&d,"middle-right","room",LJ_DOCK_TOP));
    int bottom=d.nodes[right].b;d.nodes[bottom].ratio=153.25/457;
    lj_rect area={0,60,1280,620};assert(lj_dock_layout(&d,area));
    assert(d.dividers[1].logical.y==364&&d.dividers[3].logical.y==364);
    lj_rect top=tile(&d,"top-right")->rect;lj_dock_drag drag;
    assert(lj_dock_drag_begin(&d,d.dividers[1].node,100,370,&drag)&&drag.count==2);
    assert(lj_dock_drag_move(&d,&drag,100,INT_MIN));assert(lj_dock_layout(&d,area));
    assert(d.dividers[1].logical.y==321&&d.dividers[3].logical.y==321);
    assert(!memcmp(&top,&tile(&d,"top-right")->rect,sizeof top));
    assert(lj_dock_valid(&d));
}
static void t_junction(void){
    lj_dock d;lj_dock_init(&d);assert(lj_dock_drop(&d,"top","room",LJ_DOCK_TOP));
    assert(lj_dock_drop(&d,"left","room",LJ_DOCK_LEFT));
    lj_rect area={0,60,1280,620};assert(lj_dock_layout(&d,area));
    assert(d.dividers[0].rect.w==1280&&d.dividers[1].rect.h==304);
    lj_dock_drag drag;assert(lj_dock_drag_begin(&d,d.root,100,370,&drag));assert(drag.count==1);
    assert(lj_dock_drag_move(&d,&drag,100,400));assert(lj_dock_layout(&d,area));
    lj_rect expected[]={{0,60,1280,334},{0,406,634,274},{646,406,634,274}};
    const char *ids[]={"top","left","room"};
    for(int i=0;i<3;i++)assert(!memcmp(&expected[i],&tile(&d,ids[i])->rect,sizeof(lj_rect)));
    puts("H6 topology: both orientations, every divider/all tiles, bounds, frozen membership, full-row T-junction PASS");
}
int main(void) {
    for(int f=0;f<2;f++)for(int s=0;s<2;s++)for(int n=0;n<3;n++)topology(f,s,n);
    t_junction();unequal_group_bounds();
    lj_dock d, saved, loaded;
    lj_rect area = {20, 30, 1200, 800}, old;
    char path[256], id[81];
    int i, j, divider;
    snprintf(path, sizeof path, "/tmp/liljack-dock-test-%ld", (long)getpid());
    /* Equal stacks stay equal across odd counts and resized viewports. */
    char names[63][81];const char *ids[63];
    for(i=0;i<63;i++){snprintf(names[i],81,"equal-%d",i);ids[i]=names[i];}
    for(int n=0;n<=63;n++) {
        assert(lj_dock_retile(&d,ids,n));
        for(int height=800;height<=1600;height+=137) {
            assert(lj_dock_layout(&d,(lj_rect){0,0,1400,height}));
            assert(tile(&d,"room")->rect.h==height);
            int lo=INT_MAX,hi=0;
            for(i=0;i<n;i++){lj_rect r=tile(&d,ids[i])->rect;if(r.h<lo)lo=r.h;if(r.h>hi)hi=r.h;assert(r.w==tile(&d,ids[0])->rect.w);}
            if(n&&hi-lo>1){fprintf(stderr,"unequal n=%d h=%d lo=%d hi=%d\n",n,height,lo,hi);assert(hi-lo<=1);}
        }
        assert(lj_dock_save(&d,path));assert(lj_dock_load(&loaded,path));assert(lj_dock_valid(&loaded));
    }
    saved=d;const char *duplicate[]={"dup","dup"};assert(!lj_dock_retile(&d,duplicate,2));assert(!memcmp(&d,&saved,sizeof d));
    lj_dock_init(&d); assert(lj_dock_valid(&d));
    assert(lj_dock_drop(&d, "claude-1", "room", LJ_DOCK_LEFT));
    assert(lj_dock_drop(&d, "deepseek-1", "claude-1", LJ_DOCK_BOTTOM));
    assert(lj_dock_drop(&d, "claude-2", "room", LJ_DOCK_TOP));
    assert(lj_dock_layout(&d, area)); assert(d.tile_count == 4 && d.divider_count == 3);
    assert(tile(&d, "claude-1")->rect.x == 20);
    assert(tile(&d, "deepseek-1")->rect.y > tile(&d, "claude-1")->rect.y);
    old = tile(&d, "room")->rect;
    assert(lj_dock_drop(&d, "claude-1", "room", LJ_DOCK_CENTER));
    assert(lj_dock_layout(&d, area));
    assert(!memcmp(&old, &tile(&d, "claude-1")->rect, sizeof old));
    saved = d;
    assert(!lj_dock_remove(&d, "room")); assert(!memcmp(&d, &saved, sizeof d));
    assert(!lj_dock_drop(&d, "x", "missing", LJ_DOCK_LEFT)); assert(!memcmp(&d, &saved, sizeof d));
    assert(!lj_dock_drop(&d, "x", "room", (lj_dock_edge)99));
    assert(lj_dock_drop(&d, "room", "claude-2", LJ_DOCK_LEFT));
    assert(lj_dock_remove(&d, "deepseek-1")); assert(lj_dock_layout(&d, area));
    divider = d.dividers[0].node;
    assert(lj_dock_resize(&d, divider, INT_MIN, INT_MIN));
    assert(d.nodes[divider].ratio == .15);
    assert(lj_dock_resize(&d, divider, INT_MAX, INT_MAX));
    assert(d.nodes[divider].ratio == .85);
    assert(lj_dock_layout(&d, area));
    assert(lj_dock_edge_at(area, 21, 400) == LJ_DOCK_LEFT);
    assert(lj_dock_edge_at(area, 1219, 400) == LJ_DOCK_RIGHT);
    assert(lj_dock_edge_at(area, 600, 31) == LJ_DOCK_TOP);
    assert(lj_dock_edge_at(area, 600, 829) == LJ_DOCK_BOTTOM);
    assert(lj_dock_edge_at(area, 600, 400) == LJ_DOCK_CENTER);
    for (i = 0; i < 40; i++) for (j = 0; j < 40; j++) {
        assert(lj_dock_layout(&d, (lj_rect){-20, -30, i, j}));
        for (int k = 0; k < d.tile_count; k++) {
            lj_rect r = d.tiles[k].rect;
            assert(r.w >= 0 && r.h >= 0 && r.x >= -20 && r.y >= -30);
            assert(r.x + r.w <= -20 + i && r.y + r.h <= -30 + j);
        }
    }
    assert(!lj_dock_layout(&d, (lj_rect){INT_MAX, 0, 10, 0}));
    assert(!lj_dock_layout(&d, (lj_rect){0, 0, -1, 10}));
    assert(lj_dock_save(&d, path)); lj_dock_init(&loaded); assert(lj_dock_load(&loaded, path));
    assert(lj_dock_layout(&d, area)); assert(lj_dock_layout(&loaded, area));
    assert(d.tile_count == loaded.tile_count);
    for (i = 0; i < d.tile_count; i++) {
        assert(!strcmp(d.tiles[i].id, loaded.tiles[i].id));
        assert(!memcmp(&d.tiles[i].rect, &loaded.tiles[i].rect, sizeof(lj_rect)));
    }
    bad_file(&d, path, "LJDOCK 1\nL 726f6f6d\ntrailing");
    bad_file(&d, path, "LJDOCK 1\nL 78\n");
    bad_file(&d, path, "LJDOCK 1\nS 1 nan\nL 726f6f6d\nL 78\n");
    bad_file(&d, path, "LJDOCK 1\nS 1 .5\nL 726f6f6d\nL 726f6f6d\n");
    bad_file(&d, path, "LJDOCK 1\nS 999999999999999999999999 .5\n");
    bad_file(&d, path, "LJDOCK 1\nL 726f6f6d00\n");
    bad_file(&d, path, "LJDOCK 1\nL 0a\n");
    bad_file(&d, path, "LJDOCK 1\nS 1 .5\nL 726f6f6d\n");
    lj_dock_init(&d);
    for (i = 0; i < 16; i++) { snprintf(id, sizeof id, "nested-%d", i); assert(lj_dock_drop(&d, id, "room", LJ_DOCK_LEFT)); }
    saved = d; assert(!lj_dock_drop(&d, "too-deep", "room", LJ_DOCK_LEFT)); assert(!memcmp(&saved, &d, sizeof d));
    /* Fill balanced tree to its exact pane capacity. */
    lj_dock_init(&d);
    for (i = 0; i < 63; i++) {
        char target[81];
        snprintf(id, sizeof id, "pane-%d", i);
        if (i == 0) strcpy(target, "room"); else snprintf(target, sizeof target, "pane-%d", (i - 1) / 2);
        assert(lj_dock_drop(&d, id, target, LJ_DOCK_RIGHT));
    }
    assert(lj_dock_layout(&d, area) && d.tile_count == 64);
    saved = d; assert(!lj_dock_drop(&d, "overflow", "room", LJ_DOCK_RIGHT)); assert(!memcmp(&saved, &d, sizeof d));
    assert(lj_dock_remove(&d, "pane-30")); assert(lj_dock_drop(&d, "replacement", "room", LJ_DOCK_TOP));
    /* Deterministic adversarial moves/removes keep geometry bounded and room alive. */
    srand(42);
    for (i = 0; i < 10000; i++) {
        char target[81];
        assert(lj_dock_layout(&d, area));
        strcpy(target, d.tiles[rand() % d.tile_count].id);
        snprintf(id, sizeof id, "random-%d", rand() % 80);
        if (rand() % 4 == 0) (void)lj_dock_remove(&d, id);
        else (void)lj_dock_drop(&d, id, target, (lj_dock_edge)(rand() % 5));
        assert(lj_dock_valid(&d));
    }
    remove(path);
    puts("C docking: geometry, snapping, resize, persistence, limits, 10000 mutations passed");
    return 0;
}
