/* r_accel_cron.c — Cronopio accelerated 3D path for Quake.
 *
 * When r_accel is set, R_RenderView_ calls R_AccelDrawing() instead of the
 * software edge/span pipeline. It draws the 3D scene through the host triangle
 * rasteriser (cron_polys, shared z-buffer) into the framebuffer vid.buffer
 * aliases (CRON_FB); the 2D HUD then composites on top.
 *
 *   C3.1 (here): world surfaces — perspective-correct textured triangles with
 *   Quake's per-texel lightmap (CRON_POLY_LIGHTMAP) and a z-buffer.
 *   Later: brush-model entities, alias models, sky/water, sprites/particles.
 */

#include "quakedef.h"
#include "render.h"
#include "r_local.h"
#include "model.h"
#include "client.h"
#include "mathlib.h"

#include <string.h>

#include <cronopio.h>
#include <cronopio3d.h>

extern i32   r_framecount;          /* d_iface.h */
extern i32   r_visframecount;       /* r_local.h */
extern i32   d_lightstylevalue[256];/* r_shared.h */
extern byte* host_colormap;         /* host.h: 64*256 light table */

#define ACCEL_NEAR   4.0f
#define ACCEL_FAR    8192.0f
#define MAXSURFVERTS 32             /* polygon verts per surface (fan source) */

static cron_mat4   g_mvp;
static int         g_vx, g_vy, g_vw, g_vh;   /* viewport rect */

static int32_t     g_zbuffer[CRON_SCREEN_W * CRON_SCREEN_H];
static byte        g_lmgrid[18 * 18];        /* per-surface light rows (0..63) */
static cron_vert_t g_batch[6];               /* one (clipped) triangle = up to 6 */

/* ---- view matrix from r_refdef ---------------------------------------- */

static void accel_setup_view(void) {
    vec3_t fwd, right, up;
    AngleVectors(r_refdef.viewangles, fwd, right, up);

    cron_vec3 eye = cron_v3(r_refdef.vieworg[0], r_refdef.vieworg[1], r_refdef.vieworg[2]);
    cron_vec3 tgt = cron_v3(eye.x + fwd[0], eye.y + fwd[1], eye.z + fwd[2]);
    cron_vec3 cup = cron_v3(up[0], up[1], up[2]);

    cron_mat4 view, proj;
    cron_mat_lookat(&view, eye, tgt, cup);
    float aspect = (float)g_vw / (float)g_vh;
    float fovy   = r_refdef.fov_y * (CRON_PI / 180.0f);
    cron_mat_perspective(&proj, fovy, aspect, ACCEL_NEAR, ACCEL_FAR);
    cron_mat_mul(&g_mvp, &proj, &view);
}

/* Perspective-divide a clipped vertex and map to the view RECT (not the whole
 * screen — cron_to_screen would use 320x240). Carries lightmap u/v. */
static void accel_to_screen(cron_vert_t* o, const cron_cvert* cv) {
    float iw = (cv->pos.w != 0.0f) ? 1.0f / cv->pos.w : 0.0f;
    float nx = cv->pos.x * iw, ny = cv->pos.y * iw, nz = cv->pos.z * iw;
    o->x = g_vx + cvm_f2i_sat_s((nx * 0.5f + 0.5f) * (float)g_vw);
    o->y = g_vy + cvm_f2i_sat_s((0.5f - ny * 0.5f) * (float)g_vh);
    o->z = cvm_f2i_sat_s(nz * 8388607.0f);
    o->u = cvm_f2i_sat_s(cv->u * 65536.0f);
    o->v = cvm_f2i_sat_s(cv->v * 65536.0f);
    o->w = cvm_f2i_sat_s(cv->pos.w * 65536.0f);
    o->c = 0;
    o->lu = cvm_f2i_sat_s(cv->lu * 65536.0f);
    o->lv = cvm_f2i_sat_s(cv->lv * 65536.0f);
}

/* Build the per-surface light grid (colormap rows) the way R_BuildLightMap
 * does, but one byte per lumel instead of per texel. */
__attribute__((noinline))
static void accel_build_lightmap(msurface_t* s, int smax, int tmax) {
    int size = smax * tmax;
    byte* lightmap = s->samples;

    if (!cl.worldmodel->lightdata || !lightmap) {
        memset(g_lmgrid, 0, (size_t)size);   /* row 0 = brightest */
        return;
    }

    int bl[18 * 18];
    int base = r_refdef.ambientlight << 8;
    for (int i = 0; i < size; i++) bl[i] = base;

    for (int maps = 0; maps < MAXLIGHTMAPS && s->styles[maps] != 255; maps++) {
        int scale = d_lightstylevalue[s->styles[maps]];
        for (int i = 0; i < size; i++) bl[i] += lightmap[i] * scale;
        lightmap += size;
    }

    for (int i = 0; i < size; i++) {
        int t = (255 * 256 - bl[i]) >> 2;   /* 8 - VID_CBITS(6) */
        if (t < 64) t = 64;
        int row = t >> 8;
        if (row > 63) row = 63;
        g_lmgrid[i] = (byte)row;
    }
}

/* Emit one world surface as a triangle fan. */
__attribute__((noinline))
static void accel_surface(msurface_t* s) {
    /* sky and turbulent (water/lava) need their own paths — later phases */
    if (s->flags & (SURF_DRAWSKY | SURF_DRAWTURB)) return;

    model_t*  m   = cl.worldmodel;
    texture_t* tex = s->texinfo->texture;
    if (!tex) return;

    int smax = (s->extents[0] >> 4) + 1;
    int tmax = (s->extents[1] >> 4) + 1;
    if (smax > 18) smax = 18;
    if (tmax > 18) tmax = 18;

    cron_image(0, (const uint8_t*)tex + tex->offsets[0], (int)tex->width, (int)tex->height);
    accel_build_lightmap(s, smax, tmax);
    cron_lightmap(g_lmgrid, smax, tmax);

    /* gather polygon verts + per-vertex attrs */
    cron_vec3  P[MAXSURFVERTS];
    cron_cvert A[MAXSURFVERTS];
    int nv = s->numedges;
    if (nv > MAXSURFVERTS) nv = MAXSURFVERTS;

    const float* vs = s->texinfo->vecs[0];
    const float* vt = s->texinfo->vecs[1];

    for (int i = 0; i < nv; i++) {
        int e = m->surfedges[s->firstedge + i];
        mvertex_t* mv = (e >= 0) ? &m->vertexes[m->edges[e].v[0]]
                                 : &m->vertexes[m->edges[-e].v[1]];
        float* pos = mv->position;
        P[i] = cron_v3(pos[0], pos[1], pos[2]);

        float ss = pos[0]*vs[0] + pos[1]*vs[1] + pos[2]*vs[2] + vs[3];
        float tt = pos[0]*vt[0] + pos[1]*vt[1] + pos[2]*vt[2] + vt[3];
        A[i].u  = ss;
        A[i].v  = tt;
        A[i].lu = (ss - (float)s->texturemins[0]) * (1.0f / 16.0f);
        A[i].lv = (tt - (float)s->texturemins[1]) * (1.0f / 16.0f);
        A[i].light = 0;
    }

    const int mode = CRON_POLY_TEX | CRON_POLY_LIGHTMAP | CRON_POLY_PERSP | CRON_POLY_ZTEST;

    /* fan: (0, i, i+1). Transform + near-clip + project each, then submit. */
    for (int i = 1; i < nv - 1; i++) {
        cron_cvert tri[3], clipped[6];
        cron_mat_point(&tri[0].pos, &g_mvp, P[0]);   tri[0].u=A[0].u;   tri[0].v=A[0].v;   tri[0].lu=A[0].lu;   tri[0].lv=A[0].lv;   tri[0].light=0;
        cron_mat_point(&tri[1].pos, &g_mvp, P[i]);   tri[1].u=A[i].u;   tri[1].v=A[i].v;   tri[1].lu=A[i].lu;   tri[1].lv=A[i].lv;   tri[1].light=0;
        cron_mat_point(&tri[2].pos, &g_mvp, P[i+1]); tri[2].u=A[i+1].u; tri[2].v=A[i+1].v; tri[2].lu=A[i+1].lu; tri[2].lv=A[i+1].lv; tri[2].light=0;
        int n = cron_clip_near(tri, clipped);
        for (int j = 0; j < n; j++) accel_to_screen(&g_batch[j], &clipped[j]);
        if (n) cron_polys(mode, g_batch, n, 0, -1);
    }
}

/* BSP walk: PVS cull (node->visframe), mark visible leaf surfaces, render
 * front-facing marked surfaces — mirrors R_RecursiveWorldNode. */
__attribute__((noinline))
static void accel_node(mnode_t* node) {
    if (node->contents == CONTENTS_SOLID) return;
    if (node->visframe != r_visframecount) return;

    if (node->contents < 0) {   /* leaf: mark its surfaces visible */
        mleaf_t* leaf = (mleaf_t*)node;
        msurface_t** mark = leaf->firstmarksurface;
        int c = leaf->nummarksurfaces;
        while (c-- > 0) { (*mark)->visframe = r_framecount; mark++; }
        return;
    }

    mplane_t* plane = node->plane;
    float dot = r_refdef.vieworg[0]*plane->normal[0]
              + r_refdef.vieworg[1]*plane->normal[1]
              + r_refdef.vieworg[2]*plane->normal[2] - plane->dist;
    int side = (dot < 0.0f);

    accel_node(node->children[side]);

    int c = node->numsurfaces;
    msurface_t* surf = cl.worldmodel->surfaces + node->firstsurface;
    int wantback = (dot < 0.0f) ? SURF_PLANEBACK : 0;
    for (; c > 0; c--, surf++) {
        if (surf->visframe != r_framecount) continue;
        if ((surf->flags & SURF_PLANEBACK) != wantback) continue;
        accel_surface(surf);
    }

    accel_node(node->children[!side]);
}

/* ---- entry ------------------------------------------------------------- */

void R_AccelDrawing(void) {
    g_vx = r_refdef.vrect.x;
    g_vy = r_refdef.vrect.y;
    g_vw = r_refdef.vrect.width;
    g_vh = r_refdef.vrect.height;
    if (g_vw <= 0 || g_vh <= 0 || !cl.worldmodel) return;

    /* Clear only the view rect (HUD owns the rest). */
    for (int y = 0; y < g_vh; y++)
        memset(vid.buffer + (size_t)(g_vy + y) * vid.width + g_vx, 0, (size_t)g_vw);

    /* z-buffer + colormap bindings; clip the rasteriser to the viewport. */
    cron_zbuf(g_zbuffer);
    cron_zclear(0x7FFFFFFF);
    cron_colormap(host_colormap, 64);
    cron_clip(g_vx, g_vy, g_vw, g_vh);

    accel_setup_view();
    accel_node(cl.worldmodel->nodes);

    cron_clip_reset();   /* restore full-screen clip for the HUD */
    cron_zbuf(0);
}
