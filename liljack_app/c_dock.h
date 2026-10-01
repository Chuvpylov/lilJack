#ifndef LILJACK_C_DOCK_H
#define LILJACK_C_DOCK_H
#define LJ_DOCK_MAX_LEAVES 64
#define LJ_DOCK_MAX_NODES 127
#define LJ_DOCK_ID_SIZE 81
/* All functions return 1 on success, 0 on invalid input/capacity/I/O failure.
 * Failed mutations/load leave the old tree untouched. Layout IDs and node indices
 * remain valid until a tree mutation; call layout again after every mutation. */
typedef struct { int x, y, w, h; } lj_rect;
typedef enum { LJ_DOCK_CENTER, LJ_DOCK_LEFT, LJ_DOCK_RIGHT,
               LJ_DOCK_TOP, LJ_DOCK_BOTTOM } lj_dock_edge;
typedef struct {
    int used, axis, a, b; /* axis: 0 leaf, 1 horizontal, 2 vertical */
    double ratio;
    char id[LJ_DOCK_ID_SIZE];
} lj_dock_node;
typedef struct { char id[LJ_DOCK_ID_SIZE]; lj_rect rect; } lj_dock_tile;
typedef struct { int node; lj_rect rect, area; int group; lj_rect logical; } lj_dock_divider;
/* A gesture freezes group membership, pointer offset and its common bounds.
 * Invalid after a tree mutation or viewport change; begin again then. */
typedef struct {
    int count, axis, pointer, low, high;
    int node[LJ_DOCK_MAX_LEAVES-1], first[LJ_DOCK_MAX_LEAVES-1];
    int span[LJ_DOCK_MAX_LEAVES-1];
    double ratio[LJ_DOCK_MAX_LEAVES-1];
} lj_dock_drag;
typedef struct {
    lj_dock_node nodes[LJ_DOCK_MAX_NODES];
    int root, tile_count, divider_count;
    lj_dock_tile tiles[LJ_DOCK_MAX_LEAVES];
    lj_dock_divider dividers[LJ_DOCK_MAX_LEAVES - 1];
} lj_dock;
void lj_dock_init(lj_dock *dock); /* initializes room */
/* Equal terminal stack on the left, full-height room on the right. Atomic. */
int lj_dock_retile(lj_dock *dock, const char *const *ids, int count);
int lj_dock_drop(lj_dock *dock, const char *leaf, const char *target, lj_dock_edge edge);
int lj_dock_remove(lj_dock *dock, const char *leaf); /* room cannot be removed */
int lj_dock_layout(lj_dock *dock, lj_rect area);
int lj_dock_resize(lj_dock *dock, int node, int x, int y); /* uses last layout */
int lj_dock_drag_begin(const lj_dock *dock, int node, int x, int y, lj_dock_drag *drag);
int lj_dock_drag_move(lj_dock *dock, const lj_dock_drag *drag, int x, int y);
lj_dock_edge lj_dock_edge_at(lj_rect area, int x, int y);
int lj_dock_save(const lj_dock *dock, const char *path);
int lj_dock_load(lj_dock *dock, const char *path);
int lj_dock_valid(const lj_dock *dock);
#endif
