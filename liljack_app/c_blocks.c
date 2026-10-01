#include <string.h>
#include "c_blocks.h"

const lj_block *lj_block_for(const lj_block *blocks, int count, const char *kind)
{
    const lj_block *fallback = NULL;
    if (!blocks || count <= 0) return NULL;
    for (int i = 0; i < count; i++) {
        if (!blocks[i].kind) {          /* the default block, remembered not returned */
            if (!fallback) fallback = &blocks[i];
            continue;
        }
        if (kind && !strcmp(blocks[i].kind, kind)) return &blocks[i];
    }
    return fallback;
}
