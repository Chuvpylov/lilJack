#ifndef LILJACK_C_BLOCKS_H
#define LILJACK_C_BLOCKS_H
/* lilJack's UI as a FLAT LIST OF TYPED BLOCKS — the shape the knowledge store organises a
 * document in, applied to the interface (docs/superpowers/specs/
 * 2026-09-12-liljack-block-model-design.md). A block is a typed region drawn
 * into a rect by one dispatch; the draw loop is `for (block : blocks) draw`,
 * not a chain of strcmp on the tile id.
 *
 * ⚠ THE MODEL LIVES HERE, THE DRAWING DOES NOT. The per-kind draw functions
 * stay in main.c because they need its static renderer helpers (panel, text,
 * button, hit); this header therefore names NO hui or SDL type and the draw
 * pointer is deliberately `void *` on both arguments. That keeps this unit
 * compilable and testable on its own — the same reason c_dock is a unit.
 *
 * ⚠ WHAT IS *NOT* HERE, ON PURPOSE: the dock (who sits where) and the layout.
 * A block knows what it draws, never where it sits. The reviewer's split
 * (2026-09-12-liljack-block-model-design-review.md, finding 2) is the C block
 * list now, a `layout.bmd` data format later and separately, if the layout is
 * ever meant to be edited. A BMD layout nothing edits would be ceremony. */

/* Shared chrome a block takes, drawn by the loop BEFORE the block's own draw.
 * ⚠ This is not decoration-by-taste: it encodes an ordering that was previously
 * implicit in where each `continue` sat in the strcmp chain. `room` returned
 * before the × was drawn and so has never had one; `media` fell through to the
 * ROLE control but was excluded from it by name. Both facts are now DECLARED
 * per block instead of being a property of line order. */
#define LJ_CHROME_CLOSE 1u   /* the × that hides the tile */
#define LJ_CHROME_ROLE  2u   /* the ROLE ▾ menu */

typedef struct {
    const char *kind;                  /* dock tile id; NULL marks the default block */
    unsigned    chrome;                /* LJ_CHROME_* this kind takes */
    void      (*draw)(void *app, void *ctx);
} lj_block;

/* The block for `kind`, or the default block (the entry whose kind is NULL) when
 * no kind matches — which is how a terminal session tile, whose id is a session
 * id and therefore never a literal, keeps being drawn. Returns NULL only when
 * the table is empty or has no default. First match wins. */
const lj_block *lj_block_for(const lj_block *blocks, int count, const char *kind);
#endif
