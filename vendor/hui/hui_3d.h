/*
 * hui_3d.h — Simple 3D projection, mesh loading, and wireframe/flat rendering
 *
 * Single-header, stb-style. Define HUI_3D_IMPLEMENTATION in exactly one
 * translation unit before including.
 *
 * Features:
 *   - Perspective projection (configurable FOV, camera Z)
 *   - X/Y rotation
 *   - hui_mesh3d: vertex + triangle face mesh (heap-allocated)
 *   - OBJ loader: hui_mesh3d_load_obj(path)  — handles v, f, quads, comments
 *   - Built-in procedural shapes:
 *       hui_mesh3d_torus(R, r, rings, sides)
 *       hui_mesh3d_cube(size)
 *   - Wireframe draw:  hui_mesh3d_wireframe(m, vp, cam, color)
 *   - Flat shading:    hui_mesh3d_flat(m, vp, cam, light_dir, base_color)
 *     Uses painter's algorithm (Z-sort) for correct overlap.
 *   - hui_mesh3d_free(m)
 *
 * Requires: hui_math.h (hui_v3, hui_mat4, HUI_DEG2RAD, HUI_PI)
 *           hui_draw.h (hui_line, hui_triangle_fill, hui_clip_push/pop)
 *
 * Usage:
 *   #define HUI_3D_IMPLEMENTATION
 *   #include "hui_3d.h"
 *
 *   hui_mesh3d *m = hui_mesh3d_load_obj("teapot.obj");
 *   if (!m) m = hui_mesh3d_torus(1.0f, 0.4f, 32, 16);
 *
 *   hui_3d_cam cam = HUI_3D_CAM_DEFAULT;
 *   cam.ry += 0.02f;   // spin
 *   hui_mesh3d_wireframe(m, viewport, cam, CUM_ACCENT);
 *   hui_mesh3d_free(m);
 */

#ifndef HUI_3D_H
#define HUI_3D_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>

#include "hui_math.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Camera ---- */

typedef struct {
    float ry;       /* Y-axis rotation (radians) */
    float rx;       /* X-axis rotation (radians) */
    float scale;    /* uniform model scale */
    float cam_z;    /* camera distance along +Z */
    float fov_deg;  /* field of view in degrees */
} hui_3d_cam;

#define HUI_3D_CAM_DEFAULT { 0.0f, 0.3f, 1.0f, 5.0f, 60.0f }

/* ---- Mesh ---- */

typedef struct {
    float *verts;   /* n_verts * 3 floats: [x0,y0,z0, x1,y1,z1, ...] */
    int   *faces;   /* n_faces * 3 ints:   [a0,b0,c0, a1,b1,c1, ...] */
    int    n_verts;
    int    n_faces;
} hui_mesh3d;

/* ---- API ---- */

/* Load a Wavefront OBJ file (v + f lines; quads are triangulated).
 * Returns NULL on failure. Caller must free with hui_mesh3d_free(). */
hui_mesh3d *hui_mesh3d_load_obj(const char *path);

/* Procedural torus: R = major radius, r = minor radius */
hui_mesh3d *hui_mesh3d_torus(float R, float r, int rings, int sides);

/* Procedural cube: axis-aligned, centered at origin */
hui_mesh3d *hui_mesh3d_cube(float size);

/* Free a mesh created by any of the above */
void hui_mesh3d_free(hui_mesh3d *m);

/* Draw wireframe — all edges of every triangle */
void hui_mesh3d_wireframe(const hui_mesh3d *m, hui_rect vp,
                           hui_3d_cam cam, hui_color c);

/* Draw flat-shaded solid triangles (painter's algorithm Z-sort).
 * light_dir should be a unit vector (not normalized internally). */
void hui_mesh3d_flat(const hui_mesh3d *m, hui_rect vp,
                     hui_3d_cam cam, hui_v3 light_dir, hui_color base_c);

/* ---- 3D Gizmo (transform manipulator) ---- */

/* Operation bitmask — combine with | */
#define HUI_GIZMO_TRANSLATE  1
#define HUI_GIZMO_ROTATE     2
#define HUI_GIZMO_SCALE      4
#define HUI_GIZMO_ALL        7

/* Draw a transform gizmo over a 3D scene.
 * matrix: the object's 4x4 TRS matrix (modified in place if user drags).
 * vp: viewport rect, cam: camera parameters matching the rendered scene.
 * op: bitmask of HUI_GIZMO_* operations to show.
 * Returns true if the matrix was modified this frame. */
bool hui_3d_gizmo(int op, hui_mat4 *matrix, hui_rect vp, hui_3d_cam cam);

/* Returns true if the mouse is currently hovering any gizmo handle. */
bool hui_gizmo_is_over(void);

/* Returns true if any gizmo handle is currently being dragged. */
bool hui_gizmo_is_using(void);

/* ---- Implementation ---- */

#ifdef HUI_3D_IMPLEMENTATION

/* Maximum transformed vertex cache (overridable) */
#ifndef HUI_3D_MAX_VERTS
#  define HUI_3D_MAX_VERTS 16384
#endif
#ifndef HUI_3D_MAX_FACES
#  define HUI_3D_MAX_FACES 32768
#endif

/* Internal projected vertex cache */
static hui_v2i hui__3d_px[HUI_3D_MAX_VERTS];
static float   hui__3d_vz[HUI_3D_MAX_VERTS]; /* view-space Z (negative = in front) */
static float   hui__3d_wx[HUI_3D_MAX_VERTS]; /* world-space after transform */
static float   hui__3d_wy[HUI_3D_MAX_VERTS];
static float   hui__3d_wz[HUI_3D_MAX_VERTS];

/* ---- Alloc helpers ---- */

static hui_mesh3d *hui__mesh_alloc(int n_verts, int n_faces) {
    hui_mesh3d *m = (hui_mesh3d *)malloc(sizeof(hui_mesh3d));
    if (!m) return NULL;
    m->n_verts = n_verts;
    m->n_faces = n_faces;
    m->verts = (float *)malloc(sizeof(float) * (size_t)(n_verts * 3));
    m->faces = (int   *)malloc(sizeof(int)   * (size_t)(n_faces * 3));
    if (!m->verts || !m->faces) { hui_mesh3d_free(m); return NULL; }
    return m;
}

void hui_mesh3d_free(hui_mesh3d *m) {
    if (!m) return;
    free(m->verts);
    free(m->faces);
    free(m);
}

/* ---- OBJ loader ---- */

hui_mesh3d *hui_mesh3d_load_obj(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    /* Two-pass: count then fill */
    int nv = 0, nf = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == 'v' && line[1] == ' ') nv++;
        else if (line[0] == 'f' && line[1] == ' ') {
            /* count triangles: a quad = 2 tris */
            int cnt = 0;
            const char *p = line + 2;
            while (*p) {
                while (*p == ' ' || *p == '\t') p++;
                if (*p && *p != '\n' && *p != '\r') { cnt++; p++; }
                while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
            }
            nf += (cnt >= 3) ? cnt - 2 : 0; /* fan triangulation */
        }
    }
    if (nv == 0 || nf == 0) { fclose(f); return NULL; }

    hui_mesh3d *m = hui__mesh_alloc(nv, nf);
    if (!m) { fclose(f); return NULL; }

    rewind(f);
    int vi = 0, fi = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == 'v' && line[1] == ' ') {
            float x = 0, y = 0, z = 0;
            sscanf(line + 2, "%f %f %f", &x, &y, &z);
            m->verts[vi*3+0] = x;
            m->verts[vi*3+1] = y;
            m->verts[vi*3+2] = z;
            vi++;
        } else if (line[0] == 'f' && line[1] == ' ') {
            /* Parse face indices — handle "v", "v/vt", "v/vt/vn", "v//vn" */
            int idx[16]; int cnt = 0;
            char *p = line + 2;
            while (*p && cnt < 16) {
                while (*p == ' ' || *p == '\t') p++;
                if (!*p || *p == '\n' || *p == '\r') break;
                int v = 0;
                sscanf(p, "%d", &v);
                if (v < 0) v = vi + v + 1;   /* negative index */
                if (v > 0) idx[cnt++] = v - 1; /* to 0-based */
                while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
            }
            /* Fan triangulation: (0,1,2), (0,2,3), ... */
            for (int k = 1; k+1 < cnt && fi < nf; k++, fi++) {
                m->faces[fi*3+0] = idx[0];
                m->faces[fi*3+1] = idx[k];
                m->faces[fi*3+2] = idx[k+1];
            }
        }
    }
    fclose(f);
    m->n_verts = vi;
    m->n_faces = fi;
    return m;
}

/* ---- Torus ---- */

hui_mesh3d *hui_mesh3d_torus(float R, float r, int rings, int sides) {
    if (rings < 3) rings = 3;
    if (sides < 3) sides = 3;
    int nv = rings * sides;
    int nf = rings * sides * 2;
    hui_mesh3d *m = hui__mesh_alloc(nv, nf);
    if (!m) return NULL;

    for (int ri = 0; ri < rings; ri++) {
        float phi = HUI_TAU * ri / rings;
        float cp = cosf(phi), sp = sinf(phi);
        for (int si = 0; si < sides; si++) {
            float theta = HUI_TAU * si / sides;
            float ct = cosf(theta), st = sinf(theta);
            int vi = ri * sides + si;
            m->verts[vi*3+0] = (R + r * ct) * cp;
            m->verts[vi*3+1] = r * st;
            m->verts[vi*3+2] = (R + r * ct) * sp;
        }
    }

    int fi = 0;
    for (int ri = 0; ri < rings; ri++) {
        for (int si = 0; si < sides; si++) {
            int a = ri * sides + si;
            int b = ri * sides + (si + 1) % sides;
            int c = ((ri + 1) % rings) * sides + si;
            int d = ((ri + 1) % rings) * sides + (si + 1) % sides;
            m->faces[fi*3+0] = a; m->faces[fi*3+1] = b; m->faces[fi*3+2] = c; fi++;
            m->faces[fi*3+0] = b; m->faces[fi*3+1] = d; m->faces[fi*3+2] = c; fi++;
        }
    }
    return m;
}

/* ---- Cube ---- */

hui_mesh3d *hui_mesh3d_cube(float s) {
    s *= 0.5f;
    hui_mesh3d *m = hui__mesh_alloc(8, 12);
    if (!m) return NULL;
    float v[8][3] = {
        {-s,-s,-s},{+s,-s,-s},{+s,+s,-s},{-s,+s,-s},
        {-s,-s,+s},{+s,-s,+s},{+s,+s,+s},{-s,+s,+s}
    };
    for (int i = 0; i < 8; i++) {
        m->verts[i*3+0] = v[i][0];
        m->verts[i*3+1] = v[i][1];
        m->verts[i*3+2] = v[i][2];
    }
    int f[12][3] = {
        {0,1,2},{0,2,3}, /* back */
        {4,6,5},{4,7,6}, /* front */
        {0,4,5},{0,5,1}, /* bottom */
        {2,6,7},{2,7,3}, /* top */
        {0,3,7},{0,7,4}, /* left */
        {1,5,6},{1,6,2}  /* right */
    };
    for (int i = 0; i < 12; i++) {
        m->faces[i*3+0] = f[i][0];
        m->faces[i*3+1] = f[i][1];
        m->faces[i*3+2] = f[i][2];
    }
    return m;
}

/* ---- Transform + project ---- */

/* Transform all vertices, fill hui__3d_px[], hui__3d_vz[], hui__3d_w*[] */
static void hui__3d_transform(const hui_mesh3d *m, hui_rect vp, hui_3d_cam cam) {
    float cxr = cosf(cam.rx), sxr = sinf(cam.rx);
    float cyr = cosf(cam.ry), syr = sinf(cam.ry);
    float f   = (vp.w * 0.5f) / tanf(cam.fov_deg * HUI_DEG2RAD * 0.5f);

    int n = m->n_verts;
    if (n > HUI_3D_MAX_VERTS) n = HUI_3D_MAX_VERTS;

    for (int i = 0; i < n; i++) {
        float vx = m->verts[i*3+0] * cam.scale;
        float vy = m->verts[i*3+1] * cam.scale;
        float vz = m->verts[i*3+2] * cam.scale;

        /* Rotate Y */
        float rx = vx * cyr + vz * syr;
        float rz = -vx * syr + vz * cyr;
        vx = rx; vz = rz;

        /* Rotate X */
        float ry2 = vy * cxr - vz * sxr;
        float rz2 = vy * sxr + vz * cxr;
        vy = ry2; vz = rz2;

        /* World coords (post-rotation) */
        hui__3d_wx[i] = vx;
        hui__3d_wy[i] = vy;
        hui__3d_wz[i] = vz;

        /* Camera: z -= cam_z (camera at +cam_z looking at origin) */
        float cz = vz - cam.cam_z;
        hui__3d_vz[i] = cz;

        if (cz >= -0.01f) {
            /* Behind/on camera — mark as off-screen */
            hui__3d_px[i] = (hui_v2i){-32000, -32000};
            continue;
        }
        float sx = vx / (-cz) * f + vp.x + vp.w * 0.5f;
        float sy = -vy / (-cz) * f + vp.y + vp.h * 0.5f;
        hui__3d_px[i] = (hui_v2i){(int16_t)(int)sx, (int16_t)(int)sy};
    }
}

static inline bool hui__3d_on_screen(int i, hui_rect vp) {
    if (hui__3d_vz[i] >= -0.01f) return false;
    hui_v2i p = hui__3d_px[i];
    /* Allow some off-screen slack so edges aren't clipped harshly */
    int slack = 64;
    return p.x > vp.x - slack && p.x < vp.x + vp.w + slack &&
           p.y > vp.y - slack && p.y < vp.y + vp.h + slack;
}

/* ---- Wireframe ---- */

void hui_mesh3d_wireframe(const hui_mesh3d *m, hui_rect vp,
                           hui_3d_cam cam, hui_color c) {
    if (!m || m->n_faces == 0) return;
    hui__3d_transform(m, vp, cam);
    hui_clip_push(vp);
    for (int fi = 0; fi < m->n_faces && fi < HUI_3D_MAX_FACES; fi++) {
        int a = m->faces[fi*3+0];
        int b = m->faces[fi*3+1];
        int cv = m->faces[fi*3+2];
        if (a >= m->n_verts || b >= m->n_verts || cv >= m->n_verts) continue;
        if (!hui__3d_on_screen(a, vp) && !hui__3d_on_screen(b, vp) &&
            !hui__3d_on_screen(cv, vp)) continue;
        hui_v2i pa = hui__3d_px[a];
        hui_v2i pb = hui__3d_px[b];
        hui_v2i pc = hui__3d_px[cv];
        hui_line(pa.x, pa.y, pb.x, pb.y, c, 1);
        hui_line(pb.x, pb.y, pc.x, pc.y, c, 1);
        hui_line(pc.x, pc.y, pa.x, pa.y, c, 1);
    }
    hui_clip_pop();
}

/* ---- Flat shading (painter's algorithm Z-sort) ---- */

typedef struct { int fi; float z; } hui__face_z_t;
static hui__face_z_t hui__face_zsort[HUI_3D_MAX_FACES];

static int hui__face_z_cmp(const void *a, const void *b) {
    float za = ((const hui__face_z_t *)a)->z;
    float zb = ((const hui__face_z_t *)b)->z;
    return (za < zb) ? 1 : (za > zb) ? -1 : 0; /* far first */
}

void hui_mesh3d_flat(const hui_mesh3d *m, hui_rect vp,
                     hui_3d_cam cam, hui_v3 light_dir, hui_color base_c) {
    if (!m || m->n_faces == 0) return;
    hui__3d_transform(m, vp, cam);

    int nf = m->n_faces;
    if (nf > HUI_3D_MAX_FACES) nf = HUI_3D_MAX_FACES;

    /* Build sort array */
    int draw_cnt = 0;
    for (int fi = 0; fi < nf; fi++) {
        int a = m->faces[fi*3+0];
        int b = m->faces[fi*3+1];
        int cv = m->faces[fi*3+2];
        if (a >= m->n_verts || b >= m->n_verts || cv >= m->n_verts) continue;
        if (hui__3d_vz[a] >= -0.01f && hui__3d_vz[b] >= -0.01f &&
            hui__3d_vz[cv] >= -0.01f) continue;
        float z = (hui__3d_vz[a] + hui__3d_vz[b] + hui__3d_vz[cv]) / 3.0f;
        hui__face_zsort[draw_cnt].fi = fi;
        hui__face_zsort[draw_cnt].z  = z;
        draw_cnt++;
    }
    qsort(hui__face_zsort, (size_t)draw_cnt, sizeof(hui__face_z_t), hui__face_z_cmp);

    /* Normalize light direction */
    float ll = sqrtf(light_dir.x*light_dir.x + light_dir.y*light_dir.y +
                     light_dir.z*light_dir.z);
    if (ll < 1e-6f) ll = 1.0f;
    float lx = light_dir.x / ll;
    float ly = light_dir.y / ll;
    float lz = light_dir.z / ll;

    hui_clip_push(vp);
    for (int di = 0; di < draw_cnt; di++) {
        int fi = hui__face_zsort[di].fi;
        int a  = m->faces[fi*3+0];
        int b  = m->faces[fi*3+1];
        int cv = m->faces[fi*3+2];

        /* Face normal from world-space positions */
        float ex = hui__3d_wx[b] - hui__3d_wx[a];
        float ey = hui__3d_wy[b] - hui__3d_wy[a];
        float ez = hui__3d_wz[b] - hui__3d_wz[a];
        float fx = hui__3d_wx[cv] - hui__3d_wx[a];
        float fy = hui__3d_wy[cv] - hui__3d_wy[a];
        float fz = hui__3d_wz[cv] - hui__3d_wz[a];
        float nx = ey*fz - ez*fy;
        float ny = ez*fx - ex*fz;
        float nz = ex*fy - ey*fx;
        float nl = sqrtf(nx*nx + ny*ny + nz*nz);
        if (nl < 1e-8f) continue;
        nx /= nl; ny /= nl; nz /= nl;

        /* Backface cull: normal should face toward camera (+Z) */
        if (nx*0.0f + ny*0.0f + nz*(-1.0f) > 0.0f) continue;

        /* Diffuse shading */
        float diff = nx*lx + ny*ly + nz*lz;
        if (diff < 0.0f) diff = 0.0f;
        float ambient = 0.15f;
        float bright  = ambient + (1.0f - ambient) * diff;
        if (bright > 1.0f) bright = 1.0f;

        hui_color col;
        col.r = (uint8_t)(base_c.r * bright);
        col.g = (uint8_t)(base_c.g * bright);
        col.b = (uint8_t)(base_c.b * bright);
        col.a = base_c.a;

        hui_v2i pa = hui__3d_px[a];
        hui_v2i pb = hui__3d_px[b];
        hui_v2i pc = hui__3d_px[cv];
        hui_triangle_fill(pa, pb, pc, col);
    }
    hui_clip_pop();
}

/* ---- Gizmo implementation ---- */

/* Active handle IDs: 0=none, 1=tx, 2=ty, 3=tz, 4=rx, 5=ry, 6=rz, 7=sx, 8=sy, 9=sz */
static int   hui__gizmo_active  = 0;
static bool  hui__gizmo_hovered = false;
static bool  hui__gizmo_using   = false;
static float hui__gizmo_drag_sx = 0.0f; /* screen-space drag start */
static float hui__gizmo_drag_sy = 0.0f;
static float hui__gizmo_drag_ox = 0.0f; /* original matrix value snapshot */
static float hui__gizmo_drag_oy = 0.0f;
static float hui__gizmo_drag_oz = 0.0f;
HUI_MAYBE_UNUSED static float hui__gizmo_drag_ow = 0.0f; /* used for rotation angle */

/* Project a world-space point to viewport screen coords. Returns false if behind camera. */
static bool hui__gizmo_project(hui_v3 world, hui_rect vp, hui_3d_cam cam, hui_v2 *out) {
    /* Apply cam.ry (Y rotation) */
    float cyr = cosf(cam.ry), syr = sinf(cam.ry);
    float vx = world.x * cyr + world.z * syr;
    float vy = world.y;
    float vz = -world.x * syr + world.z * cyr;

    /* Apply cam.rx (X rotation) */
    float cxr = cosf(cam.rx), sxr = sinf(cam.rx);
    float vy2 = vy * cxr - vz * sxr;
    float vz2 = vy * sxr + vz * cxr;
    vy = vy2; vz = vz2;

    float cz = vz - cam.cam_z;
    if (cz >= -0.01f) return false;

    float f = (vp.w * 0.5f) / tanf(cam.fov_deg * HUI_DEG2RAD * 0.5f);
    out->x = vx / (-cz) * f + vp.x + vp.w * 0.5f;
    out->y = -vy / (-cz) * f + vp.y + vp.h * 0.5f;
    return true;
}

/* Axis unit vectors */
static const hui_v3 hui__gizmo_axes[3] = {
    {1,0,0}, {0,1,0}, {0,0,1}
};
/* Axis colors (normal / highlighted) */
static const hui_color hui__gizmo_col[3]    = {
    {220,  60,  60, 255},  /* X red   */
    { 60, 200,  60, 255},  /* Y green */
    { 60,  60, 220, 255}   /* Z blue  */
};
static const hui_color hui__gizmo_col_hi[3] = {
    {255, 200,  80, 255},
    {255, 255,  80, 255},
    { 80, 220, 255, 255}
};

/* Returns screen-space length of a world-space unit vector from origin.
 * Used to keep gizmo arrow length constant (~60px). */
static float hui__gizmo_screen_scale(hui_v3 origin, hui_rect vp, hui_3d_cam cam) {
    hui_v2 o, p;
    if (!hui__gizmo_project(origin, vp, cam, &o)) return 60.0f;
    hui_v3 shifted = {origin.x + 1.0f, origin.y, origin.z};
    if (!hui__gizmo_project(shifted, vp, cam, &p)) return 60.0f;
    float dx = p.x - o.x, dy = p.y - o.y;
    float len = sqrtf(dx*dx + dy*dy);
    return (len > 1e-3f) ? (60.0f / len) : 60.0f;
}

/* Draw an arrowhead triangle at tip pointing away from base on screen */
static void hui__gizmo_arrowhead(hui_v2 base, hui_v2 tip, hui_color c) {
    float dx = tip.x - base.x, dy = tip.y - base.y;
    float len = sqrtf(dx*dx + dy*dy);
    if (len < 1.0f) return;
    float nx = dx/len, ny = dy/len;
    float px = -ny * 5.0f, py = nx * 5.0f;
    hui_v2i a = {(int16_t)(int)(tip.x + nx*10.0f), (int16_t)(int)(tip.y + ny*10.0f)};
    hui_v2i b = {(int16_t)(int)(tip.x - px),        (int16_t)(int)(tip.y - py)};
    hui_v2i cv = {(int16_t)(int)(tip.x + px),       (int16_t)(int)(tip.y + py)};
    hui_triangle_fill(a, b, cv, c);
}

/* Draw a small filled square at a screen position (scale handle) */
static void hui__gizmo_square(hui_v2 pos, hui_color c) {
    int x = (int)pos.x, y = (int)pos.y;
    for (int dy2 = -4; dy2 <= 4; dy2++)
        hui_line(x-4, y+dy2, x+4, y+dy2, c, 1);
}

bool hui_gizmo_is_over(void)  { return hui__gizmo_hovered; }
bool hui_gizmo_is_using(void) { return hui__gizmo_using;   }

bool hui_3d_gizmo(int op, hui_mat4 *matrix, hui_rect vp, hui_3d_cam cam) {
    if (!matrix) return false;

    /* Object origin in world space (translation column) */
    hui_v3 origin = { matrix->m[12], matrix->m[13], matrix->m[14] };

    /* Scale world-unit to keep arrows ~60px on screen */
    float ws = hui__gizmo_screen_scale(origin, vp, cam);

    /* Current mouse state via global hui_ctx */
    if (!hui_g) return false;
    float mx = (float)hui_g->io.mouse_x;
    float my = (float)hui_g->io.mouse_y;
    bool  lmb_down    = (hui_g->io.mouse_btn      & 1u) != 0;
    bool  lmb_pressed = ((hui_g->io.mouse_btn      & 1u) != 0) &&
                        ((hui_g->io.mouse_btn_prev  & 1u) == 0);
    bool  lmb_released= ((hui_g->io.mouse_btn      & 1u) == 0) &&
                        ((hui_g->io.mouse_btn_prev  & 1u) != 0);

    hui__gizmo_hovered = false;
    bool modified = false;

    /* Project origin */
    hui_v2 origin_s;
    bool origin_vis = hui__gizmo_project(origin, vp, cam, &origin_s);
    if (!origin_vis) {
        hui__gizmo_using = lmb_down && hui__gizmo_using;
        return false;
    }

    /* Release drag */
    if (lmb_released) {
        hui__gizmo_active = 0;
        hui__gizmo_using  = false;
    }

    /* For each axis, compute tip screen position and handle interaction */
    hui_v2 tips[3];
    bool   tip_vis[3];
    for (int ax = 0; ax < 3; ax++) {
        hui_v3 tip_w = {
            origin.x + hui__gizmo_axes[ax].x * ws,
            origin.y + hui__gizmo_axes[ax].y * ws,
            origin.z + hui__gizmo_axes[ax].z * ws
        };
        tip_vis[ax] = hui__gizmo_project(tip_w, vp, cam, &tips[ax]);
    }

    /* Hit test: 8px radius around tip */
    int hovered_axis = -1;
    int hovered_op   = 0;  /* 1=T, 2=R, 4=S */
    if (!hui__gizmo_using) {
        for (int ax = 0; ax < 3; ax++) {
            if (!tip_vis[ax]) continue;
            float hx = 0, hy = 0;
            if (op & HUI_GIZMO_TRANSLATE) { hx = tips[ax].x; hy = tips[ax].y; }
            else if (op & HUI_GIZMO_SCALE) { hx = tips[ax].x; hy = tips[ax].y; }
            else if (op & HUI_GIZMO_ROTATE) {
                /* Rotate handle: tip of a perpendicular short radius */
                hui_v3 perp_axes[3] = {{0,1,0},{0,0,1},{1,0,0}};
                hui_v3 ring_tip_w = {
                    origin.x + perp_axes[ax].x * ws,
                    origin.y + perp_axes[ax].y * ws,
                    origin.z + perp_axes[ax].z * ws
                };
                hui_v2 ring_tip_s;
                if (!hui__gizmo_project(ring_tip_w, vp, cam, &ring_tip_s)) continue;
                hx = ring_tip_s.x; hy = ring_tip_s.y;
            }
            float ddx = mx - hx, ddy = my - hy;
            if (ddx*ddx + ddy*ddy < 64.0f) { /* 8px radius */
                hovered_axis = ax;
                if (op & HUI_GIZMO_TRANSLATE)     hovered_op = 1;
                else if (op & HUI_GIZMO_SCALE)    hovered_op = 4;
                else if (op & HUI_GIZMO_ROTATE)   hovered_op = 2;
                hui__gizmo_hovered = true;
                break;
            }
        }
        /* If multiple ops, prefer translate over rotate over scale for hit */
        if (op == HUI_GIZMO_ALL) {
            for (int ax = 0; ax < 3; ax++) {
                if (!tip_vis[ax]) continue;
                float ddx = mx - tips[ax].x, ddy = my - tips[ax].y;
                if (ddx*ddx + ddy*ddy < 64.0f) {
                    hovered_axis = ax;
                    hovered_op   = 1; /* translate arrow tip */
                    hui__gizmo_hovered = true;
                    break;
                }
            }
        }
    }

    /* Begin drag */
    if (lmb_pressed && hovered_axis >= 0 && !hui__gizmo_using) {
        hui__gizmo_drag_sx = mx;
        hui__gizmo_drag_sy = my;
        /* Snapshot translation for translate ops */
        hui__gizmo_drag_ox = matrix->m[12];
        hui__gizmo_drag_oy = matrix->m[13];
        hui__gizmo_drag_oz = matrix->m[14];
        /* For scale ops, snapshot current scale instead */
        if (hovered_op == 4) {
            hui_v3 t3s, s3s; hui_v4 r3s;
            hui_mat4_decompose(matrix, &t3s, &r3s, &s3s);
            hui__gizmo_drag_ox = s3s.x;
            hui__gizmo_drag_oy = s3s.y;
            hui__gizmo_drag_oz = s3s.z;
        }
        /* handle ID: translate=1+ax, rotate=4+ax, scale=7+ax */
        if (hovered_op == 1) hui__gizmo_active = 1 + hovered_axis;
        else if (hovered_op == 2) hui__gizmo_active = 4 + hovered_axis;
        else if (hovered_op == 4) hui__gizmo_active = 7 + hovered_axis;
        hui__gizmo_using = true;
    }

    /* Apply drag */
    if (hui__gizmo_using && hui__gizmo_active > 0 && lmb_down) {
        float ddx = mx - hui__gizmo_drag_sx;
        float ddy = my - hui__gizmo_drag_sy;
        float delta = ddx - ddy; /* signed scalar drag distance */

        if (hui__gizmo_active >= 1 && hui__gizmo_active <= 3) {
            /* Translate: map screen drag to world axis movement */
            int ax = hui__gizmo_active - 1;
            /* Estimate pixels per world unit on this axis */
            hui_v2 o2, t2;
            hui_v3 test_tip = {
                origin.x + hui__gizmo_axes[ax].x,
                origin.y + hui__gizmo_axes[ax].y,
                origin.z + hui__gizmo_axes[ax].z
            };
            if (hui__gizmo_project(origin, vp, cam, &o2) &&
                hui__gizmo_project(test_tip, vp, cam, &t2)) {
                float adx = t2.x - o2.x, ady = t2.y - o2.y;
                float alen = sqrtf(adx*adx + ady*ady);
                if (alen > 0.5f) {
                    /* Project screen drag onto axis screen direction */
                    float proj = (ddx * adx + ddy * ady) / (alen * alen);
                    float world_delta = proj; /* 1 world unit = alen px */
                    matrix->m[12] = hui__gizmo_drag_ox + hui__gizmo_axes[ax].x * world_delta;
                    matrix->m[13] = hui__gizmo_drag_oy + hui__gizmo_axes[ax].y * world_delta;
                    matrix->m[14] = hui__gizmo_drag_oz + hui__gizmo_axes[ax].z * world_delta;
                    modified = true;
                }
            }
        } else if (hui__gizmo_active >= 4 && hui__gizmo_active <= 6) {
            /* Rotate: apply rotation around the axis */
            int ax = hui__gizmo_active - 4;
            float angle = delta * 0.01f; /* radians per pixel */
            hui_v3 axis = hui__gizmo_axes[ax];
            /* Axis-angle to quaternion */
            float ha = angle * 0.5f;
            hui_v4 dq = {
                axis.x * sinf(ha),
                axis.y * sinf(ha),
                axis.z * sinf(ha),
                cosf(ha)
            };
            /* Extract current rotation from matrix, apply delta */
            hui_v3 t3, s3; hui_v4 r3;
            hui_mat4_decompose(matrix, &t3, &r3, &s3);
            hui_v4 nr = hui_quat_mul(dq, r3);
            /* Normalize */
            float qlen = sqrtf(nr.x*nr.x + nr.y*nr.y + nr.z*nr.z + nr.w*nr.w);
            if (qlen > 1e-6f) { nr.x/=qlen; nr.y/=qlen; nr.z/=qlen; nr.w/=qlen; }
            *matrix = hui_mat4_recompose(t3, nr, s3);
            /* Resnapshot drag start each frame to accumulate */
            hui__gizmo_drag_sx = mx;
            hui__gizmo_drag_sy = my;
            modified = true;
        } else if (hui__gizmo_active >= 7 && hui__gizmo_active <= 9) {
            /* Scale: uniform scale along one axis */
            int ax = hui__gizmo_active - 7;
            float scale_delta = 1.0f + delta * 0.005f;
            if (scale_delta < 0.01f) scale_delta = 0.01f;
            hui_v3 t3, s3; hui_v4 r3;
            hui_mat4_decompose(matrix, &t3, &r3, &s3);
            if (ax == 0) s3.x = hui__gizmo_drag_ox * scale_delta;
            else if (ax == 1) s3.y = hui__gizmo_drag_oy * scale_delta;
            else              s3.z = hui__gizmo_drag_oz * scale_delta;
            *matrix = hui_mat4_recompose(t3, r3, s3);
            modified = true;
        }
    }

    /* Draw handles */
    hui_clip_push(vp);
    for (int ax = 0; ax < 3; ax++) {
        if (!tip_vis[ax]) continue;
        bool is_active_ax = hui__gizmo_using && (hui__gizmo_active % 3 == ax % 3 + 1 % 3);
        bool hi = (hovered_axis == ax && !hui__gizmo_using) || is_active_ax;
        hui_color col = hi ? hui__gizmo_col_hi[ax] : hui__gizmo_col[ax];

        if (op & HUI_GIZMO_TRANSLATE) {
            hui_line((int)origin_s.x, (int)origin_s.y,
                     (int)tips[ax].x,  (int)tips[ax].y, col, 2);
            hui__gizmo_arrowhead(origin_s, tips[ax], col);
        }
        if (op & HUI_GIZMO_SCALE) {
            hui_line((int)origin_s.x, (int)origin_s.y,
                     (int)tips[ax].x,  (int)tips[ax].y, col, 2);
            hui__gizmo_square(tips[ax], col);
        }
        if (op & HUI_GIZMO_ROTATE) {
            /* Draw a partial arc (16 segments, ~120 degrees) around each axis */
            hui_v3 perp_a[3] = {{0,1,0},{0,0,1},{1,0,0}};
            hui_v3 perp_b[3] = {{0,0,1},{1,0,0},{0,1,0}};
            int seg = 20;
            float arc = HUI_PI * 0.8f;
            hui_v2 prev_s = {0,0};
            bool prev_vis = false;
            for (int si = 0; si <= seg; si++) {
                float ang = -arc*0.5f + arc * si / seg;
                float ca = cosf(ang), sa2 = sinf(ang);
                hui_v3 rpt = {
                    origin.x + (perp_a[ax].x*ca + perp_b[ax].x*sa2)*ws,
                    origin.y + (perp_a[ax].y*ca + perp_b[ax].y*sa2)*ws,
                    origin.z + (perp_a[ax].z*ca + perp_b[ax].z*sa2)*ws
                };
                hui_v2 rpt_s;
                bool rv = hui__gizmo_project(rpt, vp, cam, &rpt_s);
                if (rv && prev_vis)
                    hui_line((int)prev_s.x, (int)prev_s.y,
                             (int)rpt_s.x,  (int)rpt_s.y, col, 2);
                prev_s = rpt_s;
                prev_vis = rv;
            }
        }
    }
    /* Draw center dot */
    hui_line((int)origin_s.x-3, (int)origin_s.y,
             (int)origin_s.x+3, (int)origin_s.y,
             HUI_RGB(220,220,220), 2);
    hui_line((int)origin_s.x, (int)origin_s.y-3,
             (int)origin_s.x, (int)origin_s.y+3,
             HUI_RGB(220,220,220), 2);
    hui_clip_pop();

    return modified;
}

#endif /* HUI_3D_IMPLEMENTATION */

#ifdef __cplusplus
}
#endif

#endif /* HUI_3D_H */
