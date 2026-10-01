/*
 * hui_math.h — Math primitives for hui
 *
 * C99. No dependencies. Inline helpers.
 * Included by hui.h — do not include directly in most cases.
 */

#ifndef HUI_MATH_H
#define HUI_MATH_H

#include <stdint.h>
#include <string.h>  /* memset */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Basic types ---- */

/** @brief 2D float vector. */
typedef struct { float x, y; }             hui_v2;
/** @brief 3D float vector. */
typedef struct { float x, y, z; }          hui_v3;
/** @brief 4D float vector; also used as a quaternion (x,y,z,w). */
typedef struct { float x, y, z, w; }       hui_v4;  /* also quaternion */
/** @brief 8-bit RGBA color. All hui draw functions accept this type. */
typedef struct { uint8_t r, g, b, a; }     hui_color;
/** @brief Axis-aligned rectangle with int16 coords. x,y = top-left; w,h = size. */
typedef struct { int16_t x, y, w, h; }     hui_rect;
/** @brief Column-major 4×4 float matrix, stored as m[16] in column-major order. */
typedef struct { float m[16]; }            hui_mat4;

/** @brief Integer 2D vector — safe on Cortex-M0 / ATmega (no FPU). */
typedef struct { int16_t x, y; }           hui_v2i;

/* Fixed point Q8.8 for ATmega */
typedef int16_t hui_fix;
#define HUI_FIX(f)  ((hui_fix)((f) * 256.0f))
#define HUI_FIXF(x) ((float)(x) / 256.0f)

/* Size checks — _Static_assert (C11) or static_assert (C++11) */
/* Suppress unused-function warnings for helpers that are used only in some builds */
#ifdef __GNUC__
#  define HUI_MAYBE_UNUSED __attribute__((unused))
#else
#  define HUI_MAYBE_UNUSED
#endif

/* Size checks — _Static_assert (C11) or static_assert (C++11) */
#ifdef __cplusplus
#  define HUI__SASSERT(expr, msg) static_assert(expr, msg)
#else
#  define HUI__SASSERT(expr, msg) _Static_assert(expr, msg)
#endif
HUI__SASSERT(sizeof(hui_v2)    ==  8, "hui_v2 size");
HUI__SASSERT(sizeof(hui_v3)    == 12, "hui_v3 size");
HUI__SASSERT(sizeof(hui_v4)    == 16, "hui_v4 size");
HUI__SASSERT(sizeof(hui_color) ==  4, "hui_color size");
HUI__SASSERT(sizeof(hui_rect)  ==  8, "hui_rect size");
HUI__SASSERT(sizeof(hui_mat4)  == 64, "hui_mat4 size");
HUI__SASSERT(sizeof(hui_v2i)   ==  4, "hui_v2i size");

/* ---- Color helpers ---- */

/** @brief Construct a hui_color from 8-bit r,g,b,a components. */
#define HUI_RGBA(r,g,b,a) ((hui_color){(r),(g),(b),(a)})
/** @brief Construct an opaque hui_color from 8-bit r,g,b (alpha = 255). */
#define HUI_RGB(r,g,b)    ((hui_color){(r),(g),(b),255})

static inline hui_color hui_color_lerp(hui_color a, hui_color b, float t) {
    return HUI_RGBA(
        (uint8_t)(a.r + (b.r - a.r) * t),
        (uint8_t)(a.g + (b.g - a.g) * t),
        (uint8_t)(a.b + (b.b - a.b) * t),
        (uint8_t)(a.a + (b.a - a.a) * t)
    );
}

/* ---- Integer min/max/clamp (defined early — used by vec helpers below) ---- */

static inline int   hui_min(int a, int b)   { return a<b?a:b; }
static inline int   hui_max(int a, int b)   { return a>b?a:b; }
/** @brief Clamp integer v to [lo, hi]. */
static inline int   hui_clamp(int v, int lo, int hi)
    { return v<lo?lo:(v>hi?hi:v); }
/** @brief Clamp float v to [lo, hi]. */
static inline float hui_clampf(float v, float lo, float hi)
    { return v<lo?lo:(v>hi?hi:v); }
/** @brief Linear interpolate between floats a and b. @param t blend factor in [0,1]; 0 = a, 1 = b. */
static inline float hui_lerpf(float a, float b, float t)
    { return a + (b-a)*t; }

/* ---- Vec2 helpers ---- */

#include <math.h>

/** @brief Component-wise addition of two 2D vectors. */
static inline hui_v2 hui_v2_add(hui_v2 a, hui_v2 b)
    { return (hui_v2){a.x+b.x, a.y+b.y}; }
/** @brief Component-wise subtraction: a − b. */
static inline hui_v2 hui_v2_sub(hui_v2 a, hui_v2 b)
    { return (hui_v2){a.x-b.x, a.y-b.y}; }
/** @brief Uniform scalar scale of v by s. */
static inline hui_v2 hui_v2_scale(hui_v2 v, float s)
    { return (hui_v2){v.x*s, v.y*s}; }
static inline hui_v2 hui_v2_lerp(hui_v2 a, hui_v2 b, float t)
    { return (hui_v2){a.x+(b.x-a.x)*t, a.y+(b.y-a.y)*t}; }
/** @brief Dot product of two 2D vectors. */
static inline float  hui_v2_dot(hui_v2 a, hui_v2 b)
    { return a.x*b.x + a.y*b.y; }
/** @brief Euclidean length of vector v. */
static inline float  hui_v2_len(hui_v2 v)
    { return sqrtf(v.x*v.x + v.y*v.y); }
/** @brief Return unit-length direction of v; returns (0,0) if v is near-zero. */
static inline hui_v2 hui_v2_norm(hui_v2 v) {
    float l = hui_v2_len(v);
    return l > 1e-6f ? (hui_v2){v.x/l, v.y/l} : (hui_v2){0,0};
}
static inline hui_v2 hui_v2_rot(hui_v2 v, float rad) {
    float c = cosf(rad), s = sinf(rad);
    return (hui_v2){v.x*c - v.y*s, v.x*s + v.y*c};
}
static inline hui_v2 hui_v2_perp(hui_v2 v)
    { return (hui_v2){-v.y, v.x}; }
static inline float  hui_v2_mag2(hui_v2 v)
    { return v.x*v.x + v.y*v.y; }
static inline float  hui_v2_cross(hui_v2 a, hui_v2 b)
    { return a.x*b.y - a.y*b.x; }
static inline hui_v2 hui_v2_closest_on_seg(hui_v2 p, hui_v2 a, hui_v2 b) {
    hui_v2 d = hui_v2_sub(b, a);
    float m2 = hui_v2_mag2(d);
    if (m2 < 1e-12f) return a;
    float u = hui_clampf(hui_v2_dot(d, hui_v2_sub(p, a)) / m2, 0.0f, 1.0f);
    return hui_v2_add(a, hui_v2_scale(d, u));
}
/* Parametric line-line intersection. Returns true if segments intersect.
 * t is the parameter along segment a (0=a0, 1=a1). */
static inline int hui_line_intersect(hui_v2 a0, hui_v2 a1,
                                     hui_v2 b0, hui_v2 b1, float *t) {
    hui_v2 da = hui_v2_sub(a1, a0);
    hui_v2 db = hui_v2_sub(b1, b0);
    float d = hui_v2_cross(da, db);
    if (d > -1e-5f && d < 1e-5f) return 0;  /* parallel */
    hui_v2 gap = hui_v2_sub(b0, a0);
    float uA = hui_v2_cross(gap, db) / d;
    float uB = hui_v2_cross(gap, da) / d;
    if (t) *t = uA;
    return uA >= 0.0f && uA <= 1.0f && uB >= 0.0f && uB <= 1.0f;
}

static inline int hui_circle_rect_overlap(hui_v2 center, float r, hui_rect rect) {
    float nx = hui_clampf(center.x, (float)rect.x, (float)(rect.x + rect.w));
    float ny = hui_clampf(center.y, (float)rect.y, (float)(rect.y + rect.h));
    float dx = center.x - nx, dy = center.y - ny;
    return dx*dx + dy*dy <= r*r;
}

/* ---- Vec3 helpers ---- */

static inline hui_v3 hui_v3_add(hui_v3 a, hui_v3 b)
    { return (hui_v3){a.x+b.x, a.y+b.y, a.z+b.z}; }
static inline hui_v3 hui_v3_cross(hui_v3 a, hui_v3 b) {
    return (hui_v3){a.y*b.z - a.z*b.y,
                    a.z*b.x - a.x*b.z,
                    a.x*b.y - a.y*b.x};
}
static inline float  hui_v3_dot(hui_v3 a, hui_v3 b)
    { return a.x*b.x + a.y*b.y + a.z*b.z; }

/* ---- Rect helpers ---- */

/** @brief Construct a hui_rect from integer x, y, w, h. */
static inline hui_rect hui_rect_make(int x, int y, int w, int h)
    { return (hui_rect){(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h}; }
/** @brief Returns non-zero if pixel (x,y) is strictly inside rect r. */
static inline int hui_rect_contains(hui_rect r, int x, int y)
    { return x>=r.x && y>=r.y && x<r.x+r.w && y<r.y+r.h; }
static inline hui_rect hui_rect_expand(hui_rect r, int d)
    { return (hui_rect){(int16_t)(r.x-d),(int16_t)(r.y-d),(int16_t)(r.w+2*d),(int16_t)(r.h+2*d)}; }
static inline hui_rect hui_rect_shrink(hui_rect r, int d)
    { return hui_rect_expand(r, -d); }
/* AABB vs AABB overlap (touching edges = false) */
static inline int hui_rect_rect_overlap(hui_rect a, hui_rect b) {
    return a.x < b.x+b.w && a.x+a.w > b.x &&
           a.y < b.y+b.h && a.y+a.h > b.y;
}

/* ---- 2D Geometry helpers ---- */

/* Reflect vector v over a unit normal */
static inline hui_v2 hui_v2_reflect(hui_v2 v, hui_v2 normal) {
    float d = 2.0f * hui_v2_dot(v, normal);
    return (hui_v2){v.x - d*normal.x, v.y - d*normal.y};
}

/* Bounding rect of a point set */
static inline hui_rect hui_poly_envelope_rect(const hui_v2 *pts, int n) {
    if (n <= 0) return hui_rect_make(0,0,0,0);
    float x0=pts[0].x, y0=pts[0].y, x1=pts[0].x, y1=pts[0].y;
    for (int i=1;i<n;i++) {
        if (pts[i].x<x0) { x0=pts[i].x; } if (pts[i].x>x1) { x1=pts[i].x; }
        if (pts[i].y<y0) { y0=pts[i].y; } if (pts[i].y>y1) { y1=pts[i].y; }
    }
    return hui_rect_make((int)x0,(int)y0,(int)(x1-x0),(int)(y1-y0));
}

/* Point-in-convex-polygon (winding sign test) */
static inline int hui_poly_contains(const hui_v2 *pts, int n, hui_v2 p) {
    for (int i=0;i<n;i++) {
        hui_v2 a=pts[i], b=pts[(i+1)%n];
        if (hui_v2_cross(hui_v2_sub(b,a), hui_v2_sub(p,a)) < 0.0f) return 0;
    }
    return 1;
}

/* Point-in-triangle */
static inline int hui_triangle_contains(hui_v2 a, hui_v2 b, hui_v2 c, hui_v2 p) {
    hui_v2 pts[3] = {a,b,c};
    return hui_poly_contains(pts, 3, p);
}

typedef struct { hui_v2 origin, dir; } hui_ray2;

/* Ray vs AABB. Returns 1 if hit, sets *t to entry distance (can be negative = behind). */
static inline int hui_ray_rect_intersect(hui_ray2 ray, hui_rect rect, float *t) {
    float tmin = -1e30f, tmax = 1e30f;
    float rx = (float)rect.x, ry = (float)rect.y;
    float rw = (float)rect.w, rh = (float)rect.h;
    if (ray.dir.x != 0.0f) {
        float t1=(rx   -ray.origin.x)/ray.dir.x, t2=(rx+rw-ray.origin.x)/ray.dir.x;
        if (t1>t2) { float tmp=t1; t1=t2; t2=tmp; }
        tmin = t1>tmin ? t1 : tmin;
        tmax = t2<tmax ? t2 : tmax;
    } else if (ray.origin.x<rx || ray.origin.x>rx+rw) { return 0; }
    if (ray.dir.y != 0.0f) {
        float t1=(ry   -ray.origin.y)/ray.dir.y, t2=(ry+rh-ray.origin.y)/ray.dir.y;
        if (t1>t2) { float tmp=t1; t1=t2; t2=tmp; }
        tmin = t1>tmin ? t1 : tmin;
        tmax = t2<tmax ? t2 : tmax;
    } else if (ray.origin.y<ry || ray.origin.y>ry+rh) { return 0; }
    if (tmax < 0.0f || tmin > tmax) { return 0; }
    if (t) { *t = tmin; }
    return 1;
}

/* Ray vs circle. Returns 1 if hit, sets *t to nearest entry distance. */
static inline int hui_ray_circle_intersect(hui_ray2 ray, hui_v2 center, float r, float *t) {
    hui_v2 oc   = hui_v2_sub(ray.origin, center);
    float  b    = hui_v2_dot(oc, ray.dir);
    float  disc = b*b - (hui_v2_dot(oc,oc) - r*r);
    if (disc < 0.0f) { return 0; }
    float sq = sqrtf(disc);
    float tn = (-b - sq >= 0.0f) ? (-b - sq) : (-b + sq);
    if (tn < 0.0f) { return 0; }
    if (t) { *t = tn; }
    return 1;
}

/* Clip line segment p0→p1 against AABB [mn, mx].
 * Sets *out0 / *out1 to clipped endpoints.
 * Returns 1 if any portion is visible, 0 if entirely outside. */
static inline int hui_clip_line_seg(hui_v2 p0, hui_v2 p1,
                                     hui_v2 mn, hui_v2 mx,
                                     hui_v2 *out0, hui_v2 *out1) {
    /* Cohen-Sutherland outcodes */
    #define HUI__CS_LEFT   1
    #define HUI__CS_RIGHT  2
    #define HUI__CS_BOTTOM 4
    #define HUI__CS_TOP    8
    #define HUI__CS_CODE(p) \
        ((p.x < mn.x ? HUI__CS_LEFT  : 0) | \
         (p.x > mx.x ? HUI__CS_RIGHT : 0) | \
         (p.y < mn.y ? HUI__CS_BOTTOM: 0) | \
         (p.y > mx.y ? HUI__CS_TOP   : 0))
    int c0 = HUI__CS_CODE(p0), c1 = HUI__CS_CODE(p1);
    hui_v2 a = p0, b = p1;
    for (int i = 0; i < 8; i++) {
        if (!(c0 | c1)) { *out0 = a; *out1 = b; return 1; }
        if (c0 & c1) return 0;
        int c = c0 ? c0 : c1;
        hui_v2 p = {0,0};
        float dx = b.x-a.x, dy = b.y-a.y;
        if      (c & HUI__CS_TOP)    { p.x = a.x + dx*(mx.y-a.y)/dy; p.y = mx.y; }
        else if (c & HUI__CS_BOTTOM) { p.x = a.x + dx*(mn.y-a.y)/dy; p.y = mn.y; }
        else if (c & HUI__CS_RIGHT)  { p.y = a.y + dy*(mx.x-a.x)/dx; p.x = mx.x; }
        else                         { p.y = a.y + dy*(mn.x-a.x)/dx; p.x = mn.x; }
        if (c == c0) { a = p; c0 = HUI__CS_CODE(a); }
        else         { b = p; c1 = HUI__CS_CODE(b); }
    }
    #undef HUI__CS_LEFT
    #undef HUI__CS_RIGHT
    #undef HUI__CS_BOTTOM
    #undef HUI__CS_TOP
    #undef HUI__CS_CODE
    return 0;
}

/* ---- Snap helpers ---- */
static inline int   hui_snap_i(int v, int grid)
    { return (grid > 0) ? (v / grid) * grid : v; }
static inline float hui_snap_f(float v, float grid)
    { return (grid > 0.0f) ? floorf(v / grid) * grid : v; }

/* ---- Mat4 helpers ---- */

/** @brief Return the 4×4 identity matrix. */
static inline hui_mat4 hui_mat4_identity(void) {
    hui_mat4 m;
    memset(&m, 0, sizeof(m));
    m.m[0]=m.m[5]=m.m[10]=m.m[15]=1.0f;
    return m;
}

/* Decompose a TRS matrix into translation, rotation (quaternion), scale.
 * Assumes the matrix is composed as M = T * R * S (no shear).
 * Any of t/r/s may be NULL. */
static inline void hui_mat4_decompose(const hui_mat4 *m,
                                       hui_v3 *t, hui_v4 *r, hui_v3 *s) {
    /* Translation */
    if (t) { t->x = m->m[12]; t->y = m->m[13]; t->z = m->m[14]; }
    /* Scale = length of column vectors */
    float sx = sqrtf(m->m[0]*m->m[0] + m->m[1]*m->m[1] + m->m[2]*m->m[2]);
    float sy = sqrtf(m->m[4]*m->m[4] + m->m[5]*m->m[5] + m->m[6]*m->m[6]);
    float sz = sqrtf(m->m[8]*m->m[8] + m->m[9]*m->m[9] + m->m[10]*m->m[10]);
    if (s) { s->x = sx; s->y = sy; s->z = sz; }
    /* Rotation matrix (remove scale) */
    if (r && sx > 1e-6f && sy > 1e-6f && sz > 1e-6f) {
        float rm[9] = {
            m->m[0]/sx, m->m[1]/sx, m->m[2]/sx,
            m->m[4]/sy, m->m[5]/sy, m->m[6]/sy,
            m->m[8]/sz, m->m[9]/sz, m->m[10]/sz
        };
        /* Mat3 to quaternion (Shepperd method) */
        float trace = rm[0] + rm[4] + rm[8];
        if (trace > 0.0f) {
            float s2 = 0.5f / sqrtf(trace + 1.0f);
            r->w = 0.25f / s2;
            r->x = (rm[7] - rm[5]) * s2;
            r->y = (rm[2] - rm[6]) * s2;
            r->z = (rm[3] - rm[1]) * s2;
        } else if (rm[0] > rm[4] && rm[0] > rm[8]) {
            float s2 = 2.0f * sqrtf(1.0f + rm[0] - rm[4] - rm[8]);
            r->w = (rm[7]-rm[5])/s2; r->x = 0.25f*s2;
            r->y = (rm[1]+rm[3])/s2; r->z = (rm[2]+rm[6])/s2;
        } else if (rm[4] > rm[8]) {
            float s2 = 2.0f * sqrtf(1.0f + rm[4] - rm[0] - rm[8]);
            r->w = (rm[2]-rm[6])/s2; r->x = (rm[1]+rm[3])/s2;
            r->y = 0.25f*s2;         r->z = (rm[5]+rm[7])/s2;
        } else {
            float s2 = 2.0f * sqrtf(1.0f + rm[8] - rm[0] - rm[4]);
            r->w = (rm[3]-rm[1])/s2; r->x = (rm[2]+rm[6])/s2;
            r->y = (rm[5]+rm[7])/s2; r->z = 0.25f*s2;
        }
    }
}

/* Recompose a TRS matrix from translation, rotation quaternion, scale. */
static inline hui_mat4 hui_mat4_recompose(hui_v3 t, hui_v4 r, hui_v3 s) {
    float qx=r.x, qy=r.y, qz=r.z, qw=r.w;
    float xx=qx*qx, yy=qy*qy, zz=qz*qz;
    float xy=qx*qy, xz=qx*qz, yz=qy*qz;
    float wx=qw*qx, wy=qw*qy, wz=qw*qz;
    hui_mat4 m = hui_mat4_identity();
    m.m[0]  = (1.0f-2.0f*(yy+zz))*s.x;
    m.m[1]  = (       2.0f*(xy+wz))*s.x;
    m.m[2]  = (       2.0f*(xz-wy))*s.x;
    m.m[4]  = (       2.0f*(xy-wz))*s.y;
    m.m[5]  = (1.0f-2.0f*(xx+zz))*s.y;
    m.m[6]  = (       2.0f*(yz+wx))*s.y;
    m.m[8]  = (       2.0f*(xz+wy))*s.z;
    m.m[9]  = (       2.0f*(yz-wx))*s.z;
    m.m[10] = (1.0f-2.0f*(xx+yy))*s.z;
    m.m[12] = t.x; m.m[13] = t.y; m.m[14] = t.z;
    return m;
}

/* ---- 2D axis-aligned transform (scale + translate, no rotation) ---- */
/* Covers all viewport/plot coordinate transforms. For rotation use hui_xform2d. */

typedef struct { float sx, sy, tx, ty; } hui_xform2;

static inline hui_v2     hui_xform2_apply(hui_xform2 t, hui_v2 v)
    { return (hui_v2){v.x*t.sx + t.tx, v.y*t.sy + t.ty}; }
static inline hui_xform2 hui_xform2_inverse(hui_xform2 t)
    { return (hui_xform2){1.f/t.sx, 1.f/t.sy, -t.tx/t.sx, -t.ty/t.sy}; }

/* Full 2D affine transform: 3x3 stored as 6 floats [a,b,c,d,tx,ty].
 * Maps (x,y) → (a*x + c*y + tx,  b*x + d*y + ty)  (column-major convention). */
typedef struct { float a, b, c, d, tx, ty; } hui_xform2d;

static inline hui_xform2d hui_xform2d_identity(void) {
    return (hui_xform2d){1,0,0,1,0,0};
}
static inline hui_xform2d hui_xform2d_translate(float tx, float ty) {
    return (hui_xform2d){1,0,0,1,tx,ty};
}
static inline hui_xform2d hui_xform2d_scale(float sx, float sy) {
    return (hui_xform2d){sx,0,0,sy,0,0};
}
static inline hui_xform2d hui_xform2d_rotate(float rad) {
    float c=cosf(rad), s=sinf(rad);
    return (hui_xform2d){c,s,-s,c,0,0};
}
static inline hui_v2 hui_xform2d_apply(hui_xform2d t, hui_v2 v) {
    return (hui_v2){t.a*v.x + t.c*v.y + t.tx, t.b*v.x + t.d*v.y + t.ty};
}
/* Compose transforms: apply p first, then q */
static inline hui_xform2d hui_xform2d_mul(hui_xform2d p, hui_xform2d q) {
    return (hui_xform2d){
        p.a*q.a + p.c*q.b,
        p.b*q.a + p.d*q.b,
        p.a*q.c + p.c*q.d,
        p.b*q.c + p.d*q.d,
        p.a*q.tx + p.c*q.ty + p.tx,
        p.b*q.tx + p.d*q.ty + p.ty
    };
}
/* Inverse of an affine transform (assumes non-singular det) */
static inline hui_xform2d hui_xform2d_inverse(hui_xform2d t) {
    float det = t.a*t.d - t.b*t.c;
    if (det < 1e-10f && det > -1e-10f) return hui_xform2d_identity();
    float inv = 1.0f / det;
    hui_xform2d r;
    r.a  =  t.d * inv;
    r.b  = -t.b * inv;
    r.c  = -t.c * inv;
    r.d  =  t.a * inv;
    r.tx = (t.c*t.ty - t.d*t.tx) * inv;
    r.ty = (t.b*t.tx - t.a*t.ty) * inv;
    return r;
}

/* ---- Quaternion helpers (for IMU) ---- */

/* Rotate unit vector by quaternion */
static inline hui_v3 hui_quat_rotate(hui_v4 q, hui_v3 v) {
    /* Using sandwich product: q * v * q^-1 */
    float qx=q.x, qy=q.y, qz=q.z, qw=q.w;
    float ix =  qw*v.x + qy*v.z - qz*v.y;
    float iy =  qw*v.y + qz*v.x - qx*v.z;
    float iz =  qw*v.z + qx*v.y - qy*v.x;
    float iw = -qx*v.x - qy*v.y - qz*v.z;
    return (hui_v3){
        ix*qw + iw*(-qx) + iy*(-qz) - iz*(-qy),
        iy*qw + iw*(-qy) + iz*(-qx) - ix*(-qz),
        iz*qw + iw*(-qz) + ix*(-qy) - iy*(-qx)
    };
}

/* ---- Degree/radian ---- */

#define HUI_DEG2RAD  (3.14159265358979323846f / 180.0f)
#define HUI_RAD2DEG  (180.0f / 3.14159265358979323846f)
/** @brief Pi (π ≈ 3.14159). Half a full turn in radians. */
#define HUI_PI       3.14159265358979323846f
/** @brief Tau (τ = 2π ≈ 6.28318). One full turn in radians. */
#define HUI_TAU      6.28318530717958647692f

/* ---- Additional quaternion helpers (require HUI_DEG2RAD) ---- */

/** @brief Hamilton product of two quaternions; compose rotations as hui_quat_mul(outer, inner). */
static inline hui_v4 hui_quat_mul(hui_v4 a, hui_v4 b) {
    return (hui_v4){
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z
    };
}

/** @brief Build a unit quaternion from elevation and azimuth angles (degrees). Ry(az)·Rx(el) — useful for IMU orientation and light direction. @param el_deg elevation above horizon; @param az_deg azimuth, 0=north, clockwise. */
static inline hui_v4 hui_quat_from_el_az(float el_deg, float az_deg) {
    float half_el = el_deg * HUI_DEG2RAD * 0.5f;
    float half_az = az_deg * HUI_DEG2RAD * 0.5f;
    /* qAz: rotation around Y */
    float ax=0.0f, ay=sinf(half_az), az=0.0f, aw=cosf(half_az);
    /* qEl: rotation around X */
    float bx=sinf(half_el), by=0.0f, bz=0.0f, bw=cosf(half_el);
    /* Hamilton product qAz * qEl */
    return (hui_v4){
        aw*bx + ax*bw + ay*bz - az*by,
        aw*by - ax*bz + ay*bw + az*bx,
        aw*bz + ax*by - ay*bx + az*bw,
        aw*bw - ax*bx - ay*by - az*bz
    };
}

/** @brief Spherical linear interpolation between unit quaternions. @param t blend in [0,1]; 0 = a, 1 = b. Takes shortest arc automatically. */
static inline hui_v4 hui_quat_slerp(hui_v4 a, hui_v4 b, float t) {
    float dot = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
    /* Ensure shortest arc */
    if (dot < 0.0f) { b.x=-b.x; b.y=-b.y; b.z=-b.z; b.w=-b.w; dot=-dot; }
    if (dot > 0.9995f) {
        /* Nearly identical — linear interp + renormalise */
        hui_v4 r = { a.x+(b.x-a.x)*t, a.y+(b.y-a.y)*t,
                     a.z+(b.z-a.z)*t, a.w+(b.w-a.w)*t };
        float len = sqrtf(r.x*r.x + r.y*r.y + r.z*r.z + r.w*r.w);
        if (len > 1e-6f) { r.x/=len; r.y/=len; r.z/=len; r.w/=len; }
        return r;
    }
    float theta0 = acosf(dot);
    float theta  = theta0 * t;
    float sa     = sinf(theta);
    float sb     = sinf(theta0 - theta);
    float inv_s  = 1.0f / sinf(theta0);
    return (hui_v4){
        (a.x*sb + b.x*sa)*inv_s,
        (a.y*sb + b.y*sa)*inv_s,
        (a.z*sb + b.z*sa)*inv_s,
        (a.w*sb + b.w*sa)*inv_s
    };
}

#ifdef __cplusplus
}
#endif

#endif /* HUI_MATH_H */
