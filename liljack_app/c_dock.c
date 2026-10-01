#include "c_dock.h"
#include "hui_divider.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int id_ok(const char *s) {
    size_t n;
    if (!s) return 0;
    for (n = 0; n < LJ_DOCK_ID_SIZE; n++) {
        if (!s[n]) return n > 0;
        if ((unsigned char)s[n] < 32 || (unsigned char)s[n] == 127) return 0;
    }
    return 0;
}
static int alloc_node(lj_dock *d) {
    int i;
    for (i = 0; i < LJ_DOCK_MAX_NODES; i++) if (!d->nodes[i].used) {
        memset(&d->nodes[i], 0, sizeof d->nodes[i]);
        d->nodes[i].used = 1;
        return i;
    }
    return -1;
}
void lj_dock_init(lj_dock *d) {
    if (!d) return;
    memset(d, 0, sizeof *d);
    d->root = alloc_node(d);
    strcpy(d->nodes[d->root].id, "room");
}
static int find_leaf(const lj_dock *d, const char *id) {
    int i;
    for (i = 0; i < LJ_DOCK_MAX_NODES; i++)
        if (d->nodes[i].used && !d->nodes[i].axis && !strcmp(id, d->nodes[i].id)) return i;
    return -1;
}
static int validate_node(const lj_dock *d, int i, int depth, int *seen, int *leaves) {
    const lj_dock_node *n;
    int j;
    if (depth > 16 || i < 0 || i >= LJ_DOCK_MAX_NODES || seen[i]) return 0;
    seen[i] = 1;
    n = &d->nodes[i];
    if (!n->used) return 0;
    if (!n->axis) {
        if (!id_ok(n->id) || ++*leaves > LJ_DOCK_MAX_LEAVES) return 0;
        for (j = 0; j < i; j++)
            if (d->nodes[j].used && !d->nodes[j].axis && id_ok(d->nodes[j].id) && !strcmp(n->id, d->nodes[j].id)) return 0;
        return 1;
    }
    return (n->axis == 1 || n->axis == 2) && isfinite(n->ratio) &&
        n->ratio >= .1 && n->ratio <= .9 &&
        validate_node(d, n->a, depth + 1, seen, leaves) &&
        validate_node(d, n->b, depth + 1, seen, leaves);
}
int lj_dock_valid(const lj_dock *d) {
    int seen[LJ_DOCK_MAX_NODES] = {0}, leaves = 0, i;
    if (!d || !validate_node(d, d->root, 0, seen, &leaves)) return 0;
    for (i = 0; i < LJ_DOCK_MAX_NODES; i++) if (d->nodes[i].used && !seen[i]) return 0;
    return find_leaf(d, "room") >= 0;
}
/* Internal removal allows moving room; public transactions restore its invariant. */
static int remove_node(lj_dock *d, int i, const char *id) {
    lj_dock_node *n = &d->nodes[i];
    int a, b;
    if (!n->axis) {
        if (strcmp(n->id, id)) return i;
        n->used = 0;
        return -1;
    }
    a = remove_node(d, n->a, id);
    b = remove_node(d, n->b, id);
    if (a < 0 || b < 0) { n->used = 0; return a < 0 ? b : a; }
    n->a = a; n->b = b;
    return i;
}
static void invalidate_layout(lj_dock *d) { d->tile_count = d->divider_count = 0; }
int lj_dock_remove(lj_dock *d, const char *id) {
    lj_dock next;
    if (!d || !id_ok(id) || !lj_dock_valid(d) || !strcmp(id, "room")) return 0;
    next = *d;
    next.root = remove_node(&next, next.root, id);
    if (!lj_dock_valid(&next)) return 0;
    invalidate_layout(&next); *d = next;
    return 1;
}
int lj_dock_drop(lj_dock *d, const char *id, const char *target, lj_dock_edge edge) {
    lj_dock next;
    lj_dock_node old;
    int src, dst, a, b, first;
    char tmp[LJ_DOCK_ID_SIZE];
    if (!d || !id_ok(id) || !id_ok(target) || edge < LJ_DOCK_CENTER || edge > LJ_DOCK_BOTTOM || !lj_dock_valid(d)) return 0;
    dst = find_leaf(d, target);
    if (dst < 0) return 0;
    if (!strcmp(id, target)) return 1;
    next = *d;
    src = find_leaf(&next, id);
    if (edge == LJ_DOCK_CENTER && src >= 0) {
        strcpy(tmp, next.nodes[src].id);
        strcpy(next.nodes[src].id, next.nodes[dst].id);
        strcpy(next.nodes[dst].id, tmp);
    } else {
        next.root = remove_node(&next, next.root, id);
        dst = find_leaf(&next, target);
        if (dst < 0) return 0;
        old = next.nodes[dst];
        a = alloc_node(&next); b = alloc_node(&next);
        if (a < 0 || b < 0) return 0;
        first = edge == LJ_DOCK_LEFT || edge == LJ_DOCK_TOP;
        next.nodes[first ? b : a] = old;
        strcpy(next.nodes[first ? a : b].id, id);
        next.nodes[dst].id[0] = 0;
        next.nodes[dst].axis = edge == LJ_DOCK_TOP || edge == LJ_DOCK_BOTTOM ? 2 : 1;
        next.nodes[dst].ratio = .5;
        next.nodes[dst].a = a; next.nodes[dst].b = b;
    }
    if (!lj_dock_valid(&next)) return 0;
    invalidate_layout(&next); *d = next;
    return 1;
}
static int min_int(int a, int b) { return a < b ? a : b; }
static double clamp_ratio(double r) { return r < .15 ? .15 : r > .85 ? .85 : r; }
static int stack_tree(lj_dock *d, const char *const *ids, int count) {
    int node = alloc_node(d);
    if (node < 0) return -1;
    if (count == 1) { strcpy(d->nodes[node].id, ids[0]); return node; }
    int first = count / 2;
    d->nodes[node].axis = 2;
    d->nodes[node].ratio = (double)first / count;
    d->nodes[node].a = stack_tree(d, ids, first);
    d->nodes[node].b = stack_tree(d, ids + first, count - first);
    return node;
}
int lj_dock_retile(lj_dock *d, const char *const *ids, int count) {
    if (!d || count < 0 || count >= LJ_DOCK_MAX_LEAVES || (count && !ids)) return 0;
    for (int i = 0; i < count; i++) {
        if (!id_ok(ids[i]) || !strcmp(ids[i], "room")) return 0;
        for (int j = 0; j < i; j++) if (!strcmp(ids[i], ids[j])) return 0;
    }
    lj_dock next; lj_dock_init(&next);
    if (count) {
        int root = alloc_node(&next);
        next.nodes[root].axis = 1; next.nodes[root].ratio = .5;
        next.nodes[root].a = stack_tree(&next, ids, count);
        next.nodes[root].b = next.root; next.root = root;
    }
    if (!lj_dock_valid(&next)) return 0;
    *d = next; return 1;
}
static int axis_units(const lj_dock *d, int node, int axis) {
    const lj_dock_node *n = &d->nodes[node];
    return n->axis == axis ? axis_units(d,n->a,axis)+axis_units(d,n->b,axis) : 1;
}
static int gap_size(int total) { return min_int(12, total / 4); }
static void layout_node(lj_dock *d, int i, lj_rect area, int shared_axis, int shared_gap) {
    lj_dock_node *n = &d->nodes[i];
    lj_rect a = area, b = area, bar = area;
    int total, gap, span, first, minimum;
    if (!n->axis) {
        lj_dock_tile *t = &d->tiles[d->tile_count++];
        strcpy(t->id, n->id); t->rect = area;
        return;
    }
    total = n->axis == 1 ? area.w : area.h;
    int units_a = axis_units(d,n->a,n->axis), units_b = axis_units(d,n->b,n->axis);
    int equal = fabs(n->ratio-(double)units_a/(units_a+units_b)) < 1e-9;
    gap = equal ? (shared_axis == n->axis && shared_gap >= 0 ? shared_gap : min_int(12,total/(2*(units_a+units_b)-1))) : gap_size(total);
    span = total-gap;
    first = (int)(span * clamp_ratio(n->ratio));
    if (equal && total >= gap*(units_a+units_b-1)) {
        int usable = total-gap*(units_a+units_b-1);
        first = usable*units_a/(units_a+units_b)+gap*(units_a-1);
    }
    minimum = min_int(n->axis == 1 ? 140 : 110, span / 2);
    if (!equal) {
        if (first < minimum) first = minimum;
        if (first > span - minimum) first = span - minimum;
    }
    if (n->axis == 1) {
        a.w = first; b.x += first + gap; b.w = span - first;
        bar.x += first; bar.w = gap;
    } else {
        a.h = first; b.y += first + gap; b.h = span - first;
        bar.y += first; bar.h = gap;
    }
    d->dividers[d->divider_count] = (lj_dock_divider){i, bar, area, d->divider_count, bar};
    d->divider_count++;
    layout_node(d, n->a, a, n->axis, equal ? gap : -1); layout_node(d, n->b, b, n->axis, equal ? gap : -1);
}
int lj_dock_layout(lj_dock *d, lj_rect area) {
    if (!lj_dock_valid(d) || area.w < 0 || area.h < 0 ||
        area.x > INT_MAX - area.w || area.y > INT_MAX - area.h) return 0;
    invalidate_layout(d); layout_node(d, d->root, area, 0, -1);
    hui_divider_segment segments[LJ_DOCK_MAX_LEAVES-1];
    int groups[LJ_DOCK_MAX_LEAVES-1];
    for(int i=0;i<d->divider_count;i++){
        lj_dock_divider v=d->dividers[i];int axis=d->nodes[v.node].axis;
        segments[i]=(hui_divider_segment){axis,axis==1?v.rect.x:v.rect.y,
            axis==1?v.rect.y:v.rect.x,axis==1?v.rect.y+v.rect.h:v.rect.x+v.rect.w};
    }
    if(!hui_divider_groups(segments,d->divider_count,12,groups))return 0;
    for(int i=0;i<d->divider_count;i++)d->dividers[i].group=groups[i];
    return 1;
}
int lj_dock_drag_begin(const lj_dock *d,int node,int x,int y,lj_dock_drag *drag){
    if(!d||!drag)return 0;
    int selected=-1;
    for(int i=0;i<d->divider_count;i++)if(d->dividers[i].node==node)selected=i;
    if(selected<0)return 0;
    lj_dock_drag next={0};next.axis=d->nodes[node].axis;
    next.pointer=next.axis==1?x:y;next.low=INT_MIN;next.high=INT_MAX;
    for(int i=0;i<d->divider_count;i++)if(d->dividers[i].group==d->dividers[selected].group){
        lj_dock_divider v=d->dividers[i];
        int total=next.axis==1?v.area.w:v.area.h;
        int gap=next.axis==1?v.logical.w:v.logical.h,span=total-gap;
        if(span<=0)return 0;
        int first=next.axis==1?v.logical.x-v.area.x:v.logical.y-v.area.y;
        int minimum=min_int(next.axis==1?140:110,span/2);
        int lo=(int)((int64_t)span*15/100),hi=(int)((int64_t)span*85/100);
        if(lo<minimum)lo=minimum;if(hi>span-minimum)hi=span-minimum;
        /* Tiny/equal-stack panes outside the normal resize range cannot
         * represent a no-jump gesture with ratios. Leave them untouched. */
        if(first<lo||first>hi)return 0;
        if(lo-first>next.low)next.low=lo-first;
        if(hi-first<next.high)next.high=hi-first;
        int k=next.count++;next.node[k]=v.node;next.first[k]=first;next.span[k]=span;next.ratio[k]=d->nodes[v.node].ratio;
    }
    *drag=next;return next.count>0;
}
int lj_dock_drag_move(lj_dock *d,const lj_dock_drag *drag,int x,int y){
    if(!d||!drag||drag->count<1||drag->count>=LJ_DOCK_MAX_LEAVES)return 0;
    int64_t delta=(int64_t)(drag->axis==1?x:y)-drag->pointer;
    if(delta<drag->low)delta=drag->low;if(delta>drag->high)delta=drag->high;
    for(int k=0;k<drag->count;k++){
        int n=drag->node[k];
        if(n<0||n>=LJ_DOCK_MAX_NODES||!d->nodes[n].used||d->nodes[n].axis!=drag->axis||drag->span[k]<=0)return 0;
    }
    for(int k=0;k<drag->count;k++){
        /* Bias inside the desired integer pixel to avoid FP truncation drift. */
        d->nodes[drag->node[k]].ratio=delta==0?drag->ratio[k]:((double)drag->first[k]+(double)delta+0.25)/drag->span[k];
    }
    return 1;
}
int lj_dock_resize(lj_dock *d, int node, int x, int y) {
    int i;
    if (!d || node < 0 || node >= LJ_DOCK_MAX_NODES || !d->nodes[node].used || !d->nodes[node].axis) return 0;
    for (i = 0; i < d->divider_count; i++) if (d->dividers[i].node == node) {
        int members=0;
        for(int j=0;j<d->divider_count;j++)members+=d->dividers[j].group==d->dividers[i].group;
        if(members>1){lj_dock_drag drag;lj_rect r=d->dividers[i].logical;
            return lj_dock_drag_begin(d,node,r.x,r.y,&drag)&&lj_dock_drag_move(d,&drag,x,y);}
        lj_rect a = d->dividers[i].area;
        int horizontal = d->nodes[node].axis == 1;
        int total = horizontal ? a.w : a.h;
        int span = total - gap_size(total);
        double offset = horizontal ? (double)x - a.x : (double)y - a.y;
        d->nodes[node].ratio = clamp_ratio(offset / (span > 0 ? span : 1));
        return 1;
    }
    return 0;
}
lj_dock_edge lj_dock_edge_at(lj_rect a, int x, int y) {
    double dx = ((double)x - a.x) / (a.w > 0 ? a.w : 1);
    double dy = ((double)y - a.y) / (a.h > 0 ? a.h : 1);
    double ds[4] = {dx, 1 - dx, dy, 1 - dy};
    int i, best = 0;
    for (i = 1; i < 4; i++) if (ds[i] < ds[best]) best = i;
    return ds[best] < .25 ? (lj_dock_edge)(best + 1) : LJ_DOCK_CENTER;
}
static int write_node(FILE *f, const lj_dock *d, int i) {
    const lj_dock_node *n = &d->nodes[i];
    const unsigned char *p;
    if (!n->axis) {
        if (fputs("L ", f) < 0) return 0;
        for (p = (const unsigned char *)n->id; *p; p++) if (fprintf(f, "%02x", *p) < 0) return 0;
        return fputc('\n', f) != EOF;
    }
    return fprintf(f, "S %d %.17g\n", n->axis, n->ratio) > 0 &&
        write_node(f, d, n->a) && write_node(f, d, n->b);
}
int lj_dock_save(const lj_dock *d, const char *path) {
    static unsigned long counter;
    char tmp[4096];
    FILE *f = NULL;
    int ok, i, length;
    if (!path || !*path || !lj_dock_valid(d)) return 0;
    /* Exclusive create prevents clobbering another writer or following symlinks.
     * rename in the destination directory gives atomic visibility, not fsync durability. */
    for (i = 0; i < 32 && !f; i++) {
        length = snprintf(tmp, sizeof tmp, "%s.%lu.%lu.tmp", path, (unsigned long)time(NULL), ++counter);
        if (length < 0 || (size_t)length >= sizeof tmp) return 0;
        f = fopen(tmp, "wx");
    }
    if (!f) return 0;
    ok = fputs("LJDOCK 1\n", f) >= 0 && write_node(f, d, d->root);
    if (fclose(f)) ok = 0;
    if (ok && rename(tmp, path)) ok = 0;
    if (!ok) remove(tmp);
    return ok;
}
static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
static int read_node(FILE *f, lj_dock *d, int depth) {
    char line[256], extra, axis_char;
    int i, axis, a, b;
    size_t n, j;
    double ratio;
    if (depth > 16 || !fgets(line, sizeof line, f)) return -1;
    i = alloc_node(d);
    if (i < 0) return -1;
    if (line[0] == 'L' && line[1] == ' ') {
        n = strlen(line);
        if (n < 5 || line[n - 1] != '\n' || (n - 3) % 2 || (n - 3) / 2 >= LJ_DOCK_ID_SIZE) return -1;
        for (j = 2; j < n - 1; j += 2) {
            a = hex_digit(line[j]); b = hex_digit(line[j + 1]);
            if (a < 0 || b < 0 || (a == 0 && b == 0)) return -1;
            d->nodes[i].id[(j - 2) / 2] = (char)(a * 16 + b);
        }
        return id_ok(d->nodes[i].id) ? i : -1;
    }
    if (sscanf(line, "S %c %lf %c", &axis_char, &ratio, &extra) != 2 ||
        (axis_char != '1' && axis_char != '2') || !isfinite(ratio) || ratio < .1 || ratio > .9 || !strchr(line, '\n')) return -1;
    axis = axis_char - '0';
    d->nodes[i].axis = axis; d->nodes[i].ratio = ratio;
    a = read_node(f, d, depth + 1); b = a < 0 ? -1 : read_node(f, d, depth + 1);
    if (a < 0 || b < 0) return -1;
    d->nodes[i].a = a; d->nodes[i].b = b;
    return i;
}
int lj_dock_load(lj_dock *d, const char *path) {
    lj_dock next;
    char header[32];
    FILE *f;
    int ok;
    if (!d || !path || !(f = fopen(path, "r"))) return 0;
    memset(&next, 0, sizeof next);
    ok = fgets(header, sizeof header, f) && !strcmp(header, "LJDOCK 1\n");
    if (ok) { next.root = read_node(f, &next, 0); ok = lj_dock_valid(&next) && fgetc(f) == EOF && !ferror(f); }
    if (fclose(f)) ok = 0;
    if (ok) *d = next;
    return ok;
}
