#ifndef HUI_DIVIDER_H
#define HUI_DIVIDER_H
#include <stdint.h>
/* Stateless connected-collinear grouping. Caller owns storage; no UI/OS state.
 * axis is 1 (vertical line) or 2 (horizontal line). start/end are along the
 * line, half-open; bridge is the maximum gap occupied by crossing separators.
 * Example: {2,100,0,200}, {2,100,208,400}, bridge=8 => one group.
 */
typedef struct { int axis,line,start,end; } hui_divider_segment;
static inline int hui_divider_groups(const hui_divider_segment *s,int count,
                                      int bridge,int *groups){
    if(count<0||bridge<0||(count&&(!s||!groups)))return 0;
    for(int i=0;i<count;i++)
        if((s[i].axis!=1&&s[i].axis!=2)||s[i].end<s[i].start)return 0;
    for(int i=0;i<count;i++)groups[i]=i;
    for(int i=0;i<count;i++)for(int j=0;j<i;j++){
        if(s[i].axis!=s[j].axis||s[i].line!=s[j].line||
           (int64_t)s[i].start>(int64_t)s[j].end+bridge||
           (int64_t)s[j].start>(int64_t)s[i].end+bridge)continue;
        int from=groups[i],to=groups[j];
        if(from>to){int tmp=from;from=to;to=tmp;}
        for(int k=0;k<count;k++)if(groups[k]==to)groups[k]=from;
    }
    return 1;
}
/* A one-pixel stroke centred inside a reserved gutter. */
typedef struct { int x,y,w,h; } hui_divider_rect;
static inline hui_divider_rect hui_divider_stroke(hui_divider_rect r,int vertical){
    if(r.w<=0||r.h<=0)return (hui_divider_rect){r.x,r.y,0,0};
    if(vertical){r.x+=r.w/2;r.w=1;}else{r.y+=r.h/2;r.h=1;}return r;
}
/* Unicode eighth-block fractions nearest half the smaller cell dimension. */
static inline uint32_t hui_divider_glyph(int vertical,int cellw,int cellh){
    if(cellw<1||cellh<1)return vertical?0x258c:0x2582;
    int dimension=vertical?cellw:cellh,small=cellw<cellh?cellw:cellh;
    int eighths=(int)(((int64_t)small*4+dimension/2)/dimension);
    if(eighths<1)eighths=1;if(eighths>8)eighths=8;
    return vertical?(uint32_t)(0x2590-eighths):(uint32_t)(0x2580+eighths);
}
#endif
