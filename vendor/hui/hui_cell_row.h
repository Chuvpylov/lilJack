#ifndef HUI_CELL_ROW_H
#define HUI_CELL_ROW_H
#include <stdint.h>
#include <limits.h>
/* Opt-in, allocation-free layout for cell-based presenters. Coordinates and
 * widths enter in CELLS, output rectangles are pixels. Use each output for
 * BOTH paint and hit testing; never round controls independently afterward.
 * Failure leaves output unchanged. Widths and gap must describe disjoint spans.
 * Example: widths={10,10,10}; hui_cell_row_layout(2,4,10,20,widths,3,1,rects).
 */
typedef struct { int x,y,w,h; } hui_cell_rect;
static inline int hui_cell_row_layout(int col,int row,int cellw,int cellh,
                                      const int *widths,int count,int gap,
                                      hui_cell_rect *out){
    if(!widths||!out||count<1||cellw<1||cellh<1||gap<0)return 0;
    int64_t cursor=col,py=(int64_t)row*cellh;
    if(py<INT_MIN||py>INT_MAX-cellh)return 0;
    for(int i=0;i<count;i++){
        if(widths[i]<1)return 0;
        int64_t px=cursor*cellw,pw=(int64_t)widths[i]*cellw;
        if(px<INT_MIN||px>INT_MAX||pw>INT_MAX||px+pw>INT_MAX)return 0;
        cursor+=widths[i];
        if(i+1<count)cursor+=gap;
        /* Bound intermediate cells before multiplication on the next pass. */
        if(cursor<INT_MIN||cursor>INT_MAX)return 0;
    }
    cursor=col;
    for(int i=0;i<count;i++){
        out[i]=(hui_cell_rect){(int)(cursor*cellw),(int)py,widths[i]*cellw,cellh};
        cursor+=(int64_t)widths[i]+gap;
    }
    return 1;
}
#endif
