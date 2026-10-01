#ifndef HUI_BORDER_H
#define HUI_BORDER_H
#include <stdint.h>
#include <limits.h>
/* Stateless decoration geometry, caller-owned state. The frame uses only
 * complete cells contained inside the pixel rectangle. No negative rounding
 * surprises, no extra partial row at the bottom. */
typedef struct { int col,row,cols,rows; } hui_border_grid;
typedef struct { int col,row,ordinal; } hui_border_point;
static inline int64_t hui__border_floor(int64_t n,int unit){
    int64_t q=n/unit;return q-(n%unit<0);
}
static inline int hui_border_cells(int x,int y,int w,int h,int cw,int ch,hui_border_grid *out){
    if(!out||w<1||h<1||cw<1||ch<1)return 0;
    int64_t right=(int64_t)x+w,bottom=(int64_t)y+h;
    if(right>INT_MAX||bottom>INT_MAX)return 0;
    int64_t left=-hui__border_floor(-(int64_t)x,cw),top=-hui__border_floor(-(int64_t)y,ch);
    int64_t cols=hui__border_floor(right,cw)-left,rows=hui__border_floor(bottom,ch)-top;
    if(cols<3||rows<3||cols>INT_MAX||rows>INT_MAX)return 0;
    *out=(hui_border_grid){(int)left,(int)top,(int)cols,(int)rows};return 1;
}
/* Each edge includes both corners. Draw indices [0,count-1) while walking
 * clockwise to avoid painting shared corners twice. At least one empty cell
 * between anchors; successive gaps differ by at most one cell. */
static inline int hui_border_edge_count(int cells){
    if(cells<3)return 0;
    int intervals=(cells-1)/4;if(intervals<1)intervals=1;
    if(cells>=5&&intervals<2)intervals=2;return intervals+1;
}
/* Candidate interior cells nearest the middle, for a text-safe presenter.
 * Invalid candidates return -1; try attempts [0,cells). At least one empty
 * cell remains beside either corner. The caller decides which cells are free. */
static inline int hui_border_interior_candidate(int cells,int attempt){
    if(cells<5||attempt<0||attempt>=cells)return -1;
    int at=(cells-1)/2+(attempt&1?(attempt+1)/2:-attempt/2);
    return at>=2&&at<=cells-3?at:-1;
}
static inline int hui_border_position(int span,int intervals,int index){
    if(span<0||intervals<1||index<0||index>intervals)return -1;
    return (int)((int64_t)index*span/intervals);
}
static inline int hui_border_anchor(int cols,int rows,int edge,int index,hui_border_point *out){
    if(!out||cols<3||rows<3||edge<0||edge>3)return 0;
    int nx=hui_border_edge_count(cols)-1,ny=hui_border_edge_count(rows)-1;
    int n=(edge&1)?ny:nx,len=(edge&1)?rows:cols;
    if(index<0||index>n)return 0;
    int at=hui_border_position(len-1,n,index);
    int ordinal=edge==0?index:edge==1?nx+index:edge==2?nx+ny+index:2*nx+ny+index;
    *out=(hui_border_point){edge==0?at:edge==1?cols-1:edge==2?cols-1-at:0,
        edge==0?0:edge==1?at:edge==2?rows-1:rows-1-at,ordinal};return 1;
}
/* Colour phase is explicit; it never changes the caller's animation clock. */
static inline unsigned hui_border_color_index(int ordinal,uint32_t phase){return (((unsigned)ordinal+phase)&3u)/2u;}
#endif
