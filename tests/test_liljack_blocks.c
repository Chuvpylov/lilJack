/* The block table's lookup. Small, but the default-fallback branch is the one
 * that keeps TERMINAL SESSION TILES on screen: their dock id is a session id,
 * never a literal kind, so every one of them reaches its draw through the
 * NULL-kind entry. A lookup that returned NULL instead would not crash — it
 * would silently stop drawing every session, which is why the fallback is
 * asserted here rather than left to the render path to discover. */
#include <stdio.h>
#include <stdlib.h>
#include "c_blocks.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); exit(1); } } while (0)
static void d_room(void *a,void *c){(void)a;(void)c;}
static void d_git(void *a,void *c){(void)a;(void)c;}
static void d_session(void *a,void *c){(void)a;(void)c;}

int main(void) {
    const lj_block table[] = {
        {"room", 0,               d_room},
        {"git",  LJ_CHROME_CLOSE, d_git},
        {NULL,   LJ_CHROME_CLOSE|LJ_CHROME_ROLE, d_session},
    };
    const int n = (int)(sizeof(table)/sizeof(table[0]));

    const lj_block *b = lj_block_for(table,n,"room");
    CHECK(b && b->draw==d_room);
    CHECK(b->chrome==0);                       /* room takes no chrome: it returned
                                                * before the × in the old chain */
    b = lj_block_for(table,n,"git");
    CHECK(b && b->draw==d_git && b->chrome==LJ_CHROME_CLOSE);

    /* the whole point: an unknown id is a session tile, not an error */
    b = lj_block_for(table,n,"s-0f3a91c4");
    CHECK(b && b->draw==d_session);
    CHECK(b->chrome==(LJ_CHROME_CLOSE|LJ_CHROME_ROLE));
    CHECK(lj_block_for(table,n,"")->draw==d_session);
    CHECK(lj_block_for(table,n,NULL)->draw==d_session);

    /* a table with no default answers honestly rather than inventing a block */
    const lj_block nodefault[] = {{"room",0,d_room}};
    CHECK(lj_block_for(nodefault,1,"room")->draw==d_room);
    CHECK(lj_block_for(nodefault,1,"git")==NULL);

    CHECK(lj_block_for(NULL,0,"room")==NULL);
    CHECK(lj_block_for(table,0,"room")==NULL);

    /* first match wins, so a duplicate kind cannot shadow the earlier entry */
    const lj_block dup[] = {{"room",0,d_room},{"room",LJ_CHROME_ROLE,d_git},{NULL,0,d_session}};
    CHECK(lj_block_for(dup,3,"room")->draw==d_room);

    puts("C blocks: kind lookup, chrome flags, session fallback, no-default honesty, first-match passed");
    return 0;
}
