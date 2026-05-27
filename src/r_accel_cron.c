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
#include "draw.h"

#include <string.h>

#include <cronopio.h>
#include <cronopio3d.h>

extern i32   r_framecount;          /* d_iface.h */
extern i32   r_visframecount;       /* r_local.h */
extern i32   d_lightstylevalue[256];/* r_shared.h */
extern byte* host_colormap;         /* host.h: 64*256 light table */
extern cvar_t r_drawviewmodel;      /* r_main.c */
extern texture_t* R_TextureAnimation(texture_t* base);   /* r_surf.c */

#define ACCEL_NEAR   4.0f
#define ACCEL_FAR    8192.0f
#define MAXSURFVERTS 32             /* polygon verts per surface (fan source) */

static cron_mat4   g_mvp;
static int         g_vx, g_vy, g_vw, g_vh;   /* viewport rect */
static vec3_t      g_vright, g_vup;          /* view basis (for camera-facing sprites) */
static float       g_frustum[6][4];          /* world-space frustum planes (a,b,c,d) */

static int32_t     g_zbuffer[CRON_SCREEN_W * CRON_SCREEN_H];
static byte        g_lmgrid[18 * 18];        /* per-surface light rows (0..63) */
static cron_vert_t g_batch[6];               /* one (clipped) triangle = up to 6 */

/* Sky split into two 128x128 layers (a Quake sky texture is 256x128: the left
 * half is the solid background, the right half the masked cloud overlay with
 * index 0 transparent). Cached per sky texture; rebuilt when it changes. */
static byte  g_skyback[128 * 128];
static byte  g_skyfront[128 * 128];
static byte  g_skytile[128 * 128];           /* per-frame composite of the two */
static void* g_sky_built_for;
static int   g_skytile_frame = -1;

/* Underwater screen warp: a copy of the rendered view, resampled back with a
 * sine displacement (see accel_warp_view), the accel equivalent of the
 * software D_WarpScreen (which warps an offscreen buffer; accel renders
 * straight to the FB, so we copy then warp in place). */
static byte  g_warpsrc[CRON_SCREEN_W * CRON_SCREEN_H];

/* ---- view matrix from r_refdef ---------------------------------------- */

static void accel_setup_view(void) {
    vec3_t fwd, right, up;
    AngleVectors(r_refdef.viewangles, fwd, right, up);
    g_vright[0]=right[0]; g_vright[1]=right[1]; g_vright[2]=right[2];
    g_vup[0]=up[0];       g_vup[1]=up[1];       g_vup[2]=up[2];

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

/* Extract the 6 world-space frustum planes from the combined MVP (rows of a
 * row-major matrix; inside = a*x+b*y+c*z+d >= 0). */
static void accel_extract_frustum(void) {
    const float* m = g_mvp.m;
    /* left/right/bottom/top/near/far = row3 ± row{0,1,2} */
    static const int rowi[6] = { 0, 0, 1, 1, 2, 2 };
    static const float sgn[6] = { 1, -1, 1, -1, 1, -1 };
    for (int p = 0; p < 6; p++) {
        int r = rowi[p]; float s = sgn[p];
        for (int k = 0; k < 4; k++)
            g_frustum[p][k] = m[12 + k] + s * m[r*4 + k];
    }
}

/* True if a node's i16 AABB is fully outside any frustum plane. */
static int accel_node_culled(mnode_t* node) {
    for (int p = 0; p < 6; p++) {
        float* f = g_frustum[p];
        float px = (f[0] >= 0.0f) ? (float)node->minmaxs[3] : (float)node->minmaxs[0];
        float py = (f[1] >= 0.0f) ? (float)node->minmaxs[4] : (float)node->minmaxs[1];
        float pz = (f[2] >= 0.0f) ? (float)node->minmaxs[5] : (float)node->minmaxs[2];
        if (f[0]*px + f[1]*py + f[2]*pz + f[3] < 0.0f) return 1;
    }
    return 0;
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

/* Add dynamic lights into the surface block-light accumulator, mirroring the
 * software R_AddDynamicLights (r_surf.c). Writes into bl[t*smax+s] in the same
 * 8.8 space as the static lightmaps, before the bound/invert/shift below. */
__attribute__((noinline))
static void accel_add_dlights(msurface_t* surf, int* bl, int smax, int tmax) {
    mtexinfo_t* tex = surf->texinfo;
    for (int lnum = 0; lnum < MAX_DLIGHTS; lnum++) {
        if (!(surf->dlightbits & (1 << lnum))) continue;   /* not lit by this */

        float rad  = cl_dlights[lnum].radius;
        float dist = DotProduct(cl_dlights[lnum].origin, surf->plane->normal)
                   - surf->plane->dist;
        rad -= (dist < 0.0f) ? -dist : dist;
        float minlight = cl_dlights[lnum].minlight;
        if (rad < minlight) continue;
        minlight = rad - minlight;

        vec3_t impact;
        for (int i = 0; i < 3; i++)
            impact[i] = cl_dlights[lnum].origin[i] - surf->plane->normal[i]*dist;

        float l0 = DotProduct(impact, tex->vecs[0]) + tex->vecs[0][3]
                 - (float)surf->texturemins[0];
        float l1 = DotProduct(impact, tex->vecs[1]) + tex->vecs[1][3]
                 - (float)surf->texturemins[1];

        for (int t = 0; t < tmax; t++) {
            int td = (int)(l1 - t * 16); if (td < 0) td = -td;
            for (int s = 0; s < smax; s++) {
                int sd = (int)(l0 - s * 16); if (sd < 0) sd = -sd;
                int d = (sd > td) ? (sd + (td >> 1)) : (td + (sd >> 1));
                if (d < minlight)
                    bl[t*smax + s] += (int)((rad - d) * 256.0f);
            }
        }
    }
}

/* Build the per-surface light grid (colormap rows) the way R_BuildLightMap
 * does, but one byte per lumel instead of per texel. */
__attribute__((noinline))
static void accel_build_lightmap(model_t* m, msurface_t* s, int smax, int tmax) {
    int size = smax * tmax;
    byte* lightmap = s->samples;

    /* Only a world WITHOUT light data is fullbright. A surface that merely has
     * no samples (lightmap==NULL) still gets the ambient floor below and ends
     * up dark — mirroring software R_BuildLightMap. (Treating no-samples as
     * fullbright lit those surfaces — metal/special textures — far too bright.) */
    if (!m->lightdata) {
        memset(g_lmgrid, 0, (size_t)size);   /* row 0 = brightest */
        return;
    }

    int bl[18 * 18];
    int base = r_refdef.ambientlight << 8;
    for (int i = 0; i < size; i++) bl[i] = base;

    if (lightmap)
        for (int maps = 0; maps < MAXLIGHTMAPS && s->styles[maps] != 255; maps++) {
            int scale = d_lightstylevalue[s->styles[maps]];
            for (int i = 0; i < size; i++) bl[i] += lightmap[i] * scale;
            lightmap += size;
        }

    /* dynamic lights (muzzle flashes, explosions, …) — mirrors the software
     * R_AddDynamicLights so accel walls flash like the edge/span pipeline. */
    if (s->dlightframe == r_framecount)
        accel_add_dlights(s, bl, smax, tmax);

    for (int i = 0; i < size; i++) {
        int t = (255 * 256 - bl[i]) >> 2;   /* 8 - VID_CBITS(6) */
        if (t < 64) t = 64;
        int row = t >> 8;
        if (row > 63) row = 63;
        g_lmgrid[i] = (byte)row;
    }
}

/* Two-layer sky (GLQuake/software style). A Quake sky texture is 256x128: the
 * LEFT half is the foreground layer (index 0 = transparent holes), the RIGHT
 * half the background shown through them. We split it once per texture, then
 * each frame composite the two at different scroll speeds into g_skytile —
 * exactly what the software R_MakeSky does — and draw the sky as ONE opaque
 * textured layer (single pass = no z-fighting between layers). */
__attribute__((noinline))
static int accel_sky_prepare(texture_t* tex) {
    if ((int)tex->width < 256 || (int)tex->height < 128) return 0;   /* not a 256x128 sky */

    if ((void*)tex != g_sky_built_for) {
        const byte* src = (const byte*)tex + tex->offsets[0];
        int tw = (int)tex->width;
        for (int i = 0; i < 128; i++)
            for (int j = 0; j < 128; j++) {
                g_skyfront[i*128 + j] = src[i*tw + j];          /* holes = 0 */
                g_skyback [i*128 + j] = src[i*tw + j + 128];    /* background */
            }
        g_sky_built_for = (void*)tex;
        g_skytile_frame = -1;
    }

    if (g_skytile_frame != r_framecount) {
        int bs = (int)(cl.time * 3.0f);    /* background: slow scroll */
        int fs = (int)(cl.time * 6.0f);    /* foreground clouds: faster */
        for (int y = 0; y < 128; y++) {
            int byo = ((y + bs) & 127) * 128, fyo = ((y + fs) & 127) * 128;
            for (int x = 0; x < 128; x++) {
                byte f = g_skyfront[fyo + ((x + fs) & 127)];
                g_skytile[y*128 + x] = f ? f : g_skyback[byo + ((x + bs) & 127)];
            }
        }
        g_skytile_frame = r_framecount;
    }
    return 1;
}

/* Emit one surface (world or brush model) as a triangle fan, transformed by
 * `mvp` (proj*view for the world; proj*view*model for a brush entity). */
__attribute__((noinline))
static void accel_surface(model_t* m, msurface_t* s, const cron_mat4* mvp) {
    int is_sky  = s->flags & SURF_DRAWSKY;
    int is_turb = s->flags & SURF_DRAWTURB;

    /* follow texture animation (flames, lights, teleporters, slipgates …).
     * Turbulent/sky surfaces don't animate via this path. */
    texture_t* tex = (is_sky || is_turb) ? s->texinfo->texture
                                         : R_TextureAnimation(s->texinfo->texture);
    if (!tex) return;

    int mode;
    if (is_sky || is_turb) {
        /* sky and water/lava are fullbright (no lightmap); identity cmap. */
        cron_cmap(0);
        mode = CRON_POLY_TEX | CRON_POLY_PERSP | CRON_POLY_ZTEST;
        if (is_sky && accel_sky_prepare(tex)) {
            /* draw the pre-composited, pre-scrolled two-layer tile */
            cron_image(0, g_skytile, 128, 128);
        } else {
            cron_image(0, (const uint8_t*)tex + tex->offsets[0], (int)tex->width, (int)tex->height);
            /* water/lava ripple (host warps texcoords per-pixel, like the
             * software D_DrawTurbulent). */
            if (is_turb) mode |= CRON_POLY_TURB;
        }
    } else {
        cron_image(0, (const uint8_t*)tex + tex->offsets[0], (int)tex->width, (int)tex->height);
        int smax = (s->extents[0] >> 4) + 1;
        int tmax = (s->extents[1] >> 4) + 1;
        if (smax > 18) smax = 18;
        if (tmax > 18) tmax = 18;
        accel_build_lightmap(m, s, smax, tmax);
        cron_lightmap(g_lmgrid, smax, tmax);
        mode = CRON_POLY_TEX | CRON_POLY_LIGHTMAP | CRON_POLY_PERSP | CRON_POLY_ZTEST;
    }

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

        if (is_sky) {
            float d0 = pos[0] - r_refdef.vieworg[0];
            float d1 = pos[1] - r_refdef.vieworg[1];
            float d2 = (pos[2] - r_refdef.vieworg[2]) * 3.0f;
            float len = cvm_fsqrt(d0*d0 + d1*d1 + d2*d2);
            len = (len > 0.0001f) ? (6.0f * 63.0f / len) : 0.0f;
            /* scroll is baked into g_skytile (two-layer composite); here we
             * only project the view direction onto the tile. */
            A[i].u = d0 * len;
            A[i].v = d1 * len;
            A[i].lu = A[i].lv = 0.0f;
        } else {
            float ss = pos[0]*vs[0] + pos[1]*vs[1] + pos[2]*vs[2] + vs[3];
            float tt = pos[0]*vt[0] + pos[1]*vt[1] + pos[2]*vt[2] + vt[3];
            A[i].u  = ss;
            A[i].v  = tt;
            A[i].lu = (ss - (float)s->texturemins[0]) * (1.0f / 16.0f);
            A[i].lv = (tt - (float)s->texturemins[1]) * (1.0f / 16.0f);
        }
        A[i].light = 0;
    }

    /* fan: (0, i, i+1). Transform + near-clip + project each, then submit. */
    for (int i = 1; i < nv - 1; i++) {
        cron_cvert tri[3], clipped[6];
        cron_mat_point(&tri[0].pos, mvp, P[0]);   tri[0].u=A[0].u;   tri[0].v=A[0].v;   tri[0].lu=A[0].lu;   tri[0].lv=A[0].lv;   tri[0].light=0;
        cron_mat_point(&tri[1].pos, mvp, P[i]);   tri[1].u=A[i].u;   tri[1].v=A[i].v;   tri[1].lu=A[i].lu;   tri[1].lv=A[i].lv;   tri[1].light=0;
        cron_mat_point(&tri[2].pos, mvp, P[i+1]); tri[2].u=A[i+1].u; tri[2].v=A[i+1].v; tri[2].lu=A[i+1].lu; tri[2].lv=A[i+1].lv; tri[2].light=0;
        int n = cron_clip_near(tri, clipped);
        for (int j = 0; j < n; j++) accel_to_screen(&g_batch[j], &clipped[j]);
        if (n) cron_polys(mode, g_batch, n, 0, -1);
    }
}

/* ---- alias models (monsters / items / weapon) ------------------------- */

extern i32 R_LightPoint(vec3_t p);   /* r_light.h */

__attribute__((noinline))
static void accel_alias(entity_t* ent) {
    model_t* model = ent->model;
    if (!model || model->type != mod_alias) return;

    /* cached data directly (see accel_sprite): avoid Mod_Extradata's Cache_Check
     * LRU walk, which can trap on a transient model in accelerated mode. */
    aliashdr_t* ph = (aliashdr_t*)model->cache.data;
    if (!ph) return;
    mdl_t*      pmdl = (mdl_t*)((byte*)ph + ph->model);

    int frame = ent->frame;
    if (frame < 0 || frame >= pmdl->numframes) frame = 0;

    trivertx_t*  verts = (trivertx_t*)((byte*)ph + ph->frames[frame].frame);
    stvert_t*    stv   = (stvert_t*)((byte*)ph + ph->stverts);
    mtriangle_t* tris  = (mtriangle_t*)((byte*)ph + ph->triangles);

    /* skin (single; for an animated skin group take the first) */
    int skinnum = ent->skinnum;
    if (skinnum < 0 || skinnum >= pmdl->numskins) skinnum = 0;
    maliasskindesc_t* sd =
        ((maliasskindesc_t*)((byte*)ph + ph->skindesc)) + skinnum;
    uint32_t skinofs = sd->skin;
    if (sd->type == ALIAS_SKIN_GROUP) {
        maliasskingroup_t* g = (maliasskingroup_t*)((byte*)ph + sd->skin);
        skinofs = g->skindescs[0].skin;
    }
    const uint8_t* skin = (const uint8_t*)ph + skinofs;
    int sw = pmdl->skinwidth, sh = pmdl->skinheight;
    cron_image(0, skin, sw, sh);

    /* flat per-model light -> a colormap row (no per-texel lightmap here) */
    int light = R_LightPoint(ent->origin);
    int row = (255 - light) >> 2;
    if (row < 0) row = 0; if (row > 63) row = 63;
    cron_cmap(host_colormap + row * 256);

    /* model->world matrix (entity rotation/translation), with the .mdl
     * byte-vertex scale + origin folded in so we feed raw verts. Columns of
     * the rotation are Quake's forward / -right / up. */
    vec3_t ang, fwd, rgt, up;
    ang[PITCH] = -ent->angles[PITCH];
    ang[YAW]   =  ent->angles[YAW];
    ang[ROLL]  =  ent->angles[ROLL];
    AngleVectors(ang, fwd, rgt, up);

    float* sc = pmdl->scale;
    float* so = pmdl->scale_origin;
    float c0[3] = { fwd[0],  fwd[1],  fwd[2] };
    float c1[3] = { -rgt[0], -rgt[1], -rgt[2] };
    float c2[3] = { up[0],   up[1],   up[2] };
    cron_mat4 M, mvpm;
    for (int a = 0; a < 3; a++) {
        M.m[a*4+0] = sc[0] * c0[a];
        M.m[a*4+1] = sc[1] * c1[a];
        M.m[a*4+2] = sc[2] * c2[a];
        M.m[a*4+3] = so[0]*c0[a] + so[1]*c1[a] + so[2]*c2[a] + ent->origin[a];
    }
    M.m[12]=0; M.m[13]=0; M.m[14]=0; M.m[15]=1.0f;
    cron_mat_mul(&mvpm, &g_mvp, &M);

    /* Perspective-correct texturing: with the Q16.16 stverts now scaled back to
     * float texels (below), perspective no longer overflows, and affine's
     * screen-space interpolation across the close viewmodel's big triangles was
     * mis-sampling skin texels — the scattered "blue specks" on weapons.
     * CLAMP texcoords: alias skins are a single sheet, so a texel past the edge
     * must clamp to the border, not wrap to the opposite (blue) edge. */
    const int mode = CRON_POLY_TEX | CRON_POLY_PERSP | CRON_POLY_ZTEST | CRON_POLY_CLAMP;

    for (int t = 0; t < pmdl->numtris; t++) {
        mtriangle_t* tri = &tris[t];
        cron_cvert v[3], clipped[6];
        for (int k = 0; k < 3; k++) {
            int vi = tri->vertindex[k];
            trivertx_t* tv = &verts[vi];
            cron_vec3 p = cron_v3((float)tv->v[0], (float)tv->v[1], (float)tv->v[2]);
            cron_mat_point(&v[k].pos, &mvpm, p);
            /* stverts are stored Q16.16 (Mod_LoadAliasModel shifts <<16); the
             * rasteriser wants float texels, so scale back down. */
            float s  = (float)stv[vi].s * (1.0f / 65536.0f);
            float tt = (float)stv[vi].t * (1.0f / 65536.0f);
            if (!tri->facesfront && stv[vi].onseam) s += (float)(sw >> 1);
            v[k].u = s; v[k].v = tt; v[k].lu = 0; v[k].lv = 0; v[k].light = 0;
        }
        int n = cron_clip_near(v, clipped);
        for (int j = 0; j < n; j++) accel_to_screen(&g_batch[j], &clipped[j]);
        if (n) cron_polys(mode, g_batch, n, 0, -1);
    }
}

/* ---- brush-model entities (doors, platforms, ammo boxes) -------------- */

__attribute__((noinline))
static void accel_brush(entity_t* ent) {
    model_t* model = ent->model;
    if (!model || model->type != mod_brush) return;
    if (model == cl.worldmodel) return;   /* world goes through accel_node */
    currententity = ent;                  /* texture animation uses ent->frame */

    int rotated = (ent->angles[0] || ent->angles[1] || ent->angles[2]);
    vec3_t fwd, rgt, up;
    cron_mat4 M, mvpm;
    if (rotated) {
        AngleVectors(ent->angles, fwd, rgt, up);
        float c0[3] = { fwd[0], fwd[1], fwd[2] };
        float c1[3] = { -rgt[0], -rgt[1], -rgt[2] };
        float c2[3] = { up[0], up[1], up[2] };
        for (int a = 0; a < 3; a++) {
            M.m[a*4+0]=c0[a]; M.m[a*4+1]=c1[a]; M.m[a*4+2]=c2[a]; M.m[a*4+3]=ent->origin[a];
        }
        M.m[12]=0; M.m[13]=0; M.m[14]=0; M.m[15]=1.0f;
    } else {
        cron_mat_identity(&M);
        M.m[3]=ent->origin[0]; M.m[7]=ent->origin[1]; M.m[11]=ent->origin[2];
    }
    cron_mat_mul(&mvpm, &g_mvp, &M);

    /* view origin in model space, for the front-facing test (translation only;
     * the z-buffer covers the rare rotated bmodel). */
    vec3_t modelorg = { r_refdef.vieworg[0]-ent->origin[0],
                        r_refdef.vieworg[1]-ent->origin[1],
                        r_refdef.vieworg[2]-ent->origin[2] };

    msurface_t* surf = model->surfaces + model->firstmodelsurface;
    for (int i = 0; i < model->nummodelsurfaces; i++, surf++) {
        mplane_t* pl = surf->plane;
        float dot = modelorg[0]*pl->normal[0] + modelorg[1]*pl->normal[1]
                  + modelorg[2]*pl->normal[2] - pl->dist;
        int wantback = (dot < 0.0f) ? SURF_PLANEBACK : 0;
        if ((surf->flags & SURF_PLANEBACK) != wantback) continue;
        accel_surface(model, surf, &mvpm);
    }
}

/* ---- sprites (explosions, flames, ...) -------------------------------- */

__attribute__((noinline))
static void accel_sprite(entity_t* ent) {
    model_t* model = ent->model;
    if (!model || model->type != mod_sprite) return;
    /* Read the cached sprite directly, exactly like the software R_DrawSprite.
     * NOT Mod_Extradata: its Cache_Check walks the cache LRU, which traps on a
     * transient sprite (e.g. s_explod.spr) whose cache entry isn't kept warm by
     * the software pipeline in accelerated mode. */
    msprite_t* ps = (msprite_t*)model->cache.data;
    if (!ps) return;

    int frame = ent->frame;
    if (frame < 0 || frame >= ps->numframes) frame = 0;
    mspriteframe_t* fr;
    if (ps->frames[frame].type == SPR_SINGLE) {
        fr = ps->frames[frame].frameptr;
    } else {
        mspritegroup_t* g = (mspritegroup_t*)ps->frames[frame].frameptr;
        fr = g->frames[0];
    }
    if (!fr) return;

    cron_image(0, (const uint8_t*)fr->pixels, fr->width, fr->height);
    cron_cmap(0);

    float* o = ent->origin;
    /* camera-facing quad from the frame's up/down/left/right offsets */
    float lf = fr->left, rt = fr->right, up = fr->up, dn = fr->down;
    cron_vec3 corner[4];
    float cu[4], cv[4];
    /* bottom-left, top-left, top-right, bottom-right */
    float ox[4] = { lf, lf, rt, rt };
    float oy[4] = { dn, up, up, dn };
    cu[0]=0; cv[0]=(float)fr->height; cu[1]=0; cv[1]=0;
    cu[2]=(float)fr->width; cv[2]=0;   cu[3]=(float)fr->width; cv[3]=(float)fr->height;
    for (int k = 0; k < 4; k++) {
        corner[k] = cron_v3(o[0] + g_vright[0]*ox[k] + g_vup[0]*oy[k],
                            o[1] + g_vright[1]*ox[k] + g_vup[1]*oy[k],
                            o[2] + g_vright[2]*ox[k] + g_vup[2]*oy[k]);
    }

    const int mode = CRON_POLY_TEX | CRON_POLY_PERSP | CRON_POLY_ZTEST;
    int idx[6] = { 0, 1, 2, 0, 2, 3 };
    cron_cvert v[3], clipped[6];
    for (int t = 0; t < 6; t += 3) {
        for (int k = 0; k < 3; k++) {
            int c = idx[t + k];
            cron_mat_point(&v[k].pos, &g_mvp, corner[c]);
            v[k].u = cu[c]; v[k].v = cv[c]; v[k].lu = 0; v[k].lv = 0; v[k].light = 0;
        }
        int n = cron_clip_near(v, clipped);
        for (int j = 0; j < n; j++) accel_to_screen(&g_batch[j], &clipped[j]);
        if (n) cron_polys(mode, g_batch, n, 0, 255);   /* index 255 transparent */
    }
}

/* ---- particles -------------------------------------------------------- */

extern particle_t* active_particles;
extern void R_UpdateParticles(void);   /* r_part.c — age/expire/advance */

__attribute__((noinline))
static void accel_particles(void) {
    cron_cmap(0);
    for (particle_t* p = active_particles; p; p = p->next) {
        cron_vec4 c;
        cron_mat_point(&c, &g_mvp, cron_v3(p->org[0], p->org[1], p->org[2]));
        if (c.z + c.w < 0.0f || c.w <= 0.0f) continue;   /* behind near plane */
        float iw = 1.0f / c.w;
        int sx = g_vx + cvm_f2i_sat_s((c.x*iw*0.5f + 0.5f) * (float)g_vw);
        int sy = g_vy + cvm_f2i_sat_s((0.5f - c.y*iw*0.5f) * (float)g_vh);
        int sz = cvm_f2i_sat_s(c.z*iw * 8388607.0f);

        /* a 2x2 screen quad (uniform size, like the software particle) */
        int x0=sx, y0=sy, x1=sx+2, y1=sy+2;
        int qx[6] = { x0, x1, x1, x0, x1, x0 };
        int qy[6] = { y0, y0, y1, y0, y1, y1 };
        for (int k = 0; k < 6; k++) {
            g_batch[k].x = qx[k]; g_batch[k].y = qy[k]; g_batch[k].z = sz;
            g_batch[k].u = g_batch[k].v = g_batch[k].w = g_batch[k].c = 0;
            g_batch[k].lu = g_batch[k].lv = 0;
        }
        cron_polys(CRON_POLY_FLAT | CRON_POLY_ZTEST, g_batch, 6, (int)p->color, -1);
    }
    /* Software R_DrawParticles ages + expires particles; the accel path draws
     * them itself, so run the same bookkeeping here or they'd live forever. */
    R_UpdateParticles();
}

/* ---- underwater screen warp ------------------------------------------- */

extern i32   intsintable[];   /* r_shared.h — sine table, range [0, 2*AMP2] */

/* The accel equivalent of D_WarpScreen: the software renderer warps an
 * offscreen buffer into the framebuffer; we render straight to the FB, so copy
 * the view rect out and resample it back with the same sine displacement. Only
 * run underwater. Mirrors d_scan.c: dest[v][u] = src[rowofs[v+turb[u]]]
 * [col[turb[v]+u]], with a slight compression so the offset never reads past
 * the edge. */
__attribute__((noinline))
static void accel_warp_view(void) {
    int w = g_vw, h = g_vh;
    if (w <= AMP2*2 || h <= AMP2*2) return;

    for (int v = 0; v < h; v++)
        memcpy(g_warpsrc + (size_t)v*w,
               (const void*)(vid.buffer + (size_t)(g_vy+v)*vid.width + g_vx), (size_t)w);

    static int col[CRON_SCREEN_W + AMP2*2];
    static int rowo[CRON_SCREEN_H + AMP2*2];
    for (int u = 0; u < w + AMP2*2; u++) col[u]  = (int)((float)u * w / (w + AMP2*2));
    for (int v = 0; v < h + AMP2*2; v++) rowo[v] = (int)((float)v * h / (h + AMP2*2));

    i32* turb = intsintable + ((i32)(cl.time * SPEED) & (CYCLE - 1));

    for (int v = 0; v < h; v++) {
        volatile uint8_t* dest = vid.buffer + (size_t)(g_vy+v)*vid.width + g_vx;
        int* colp = &col[turb[v]];
        for (int u = 0; u < w; u++)
            dest[u] = g_warpsrc[(size_t)rowo[v + turb[u]]*w + colp[u]];
    }
}

/* BSP walk: PVS cull (node->visframe), mark visible leaf surfaces, render
 * front-facing marked surfaces — mirrors R_RecursiveWorldNode. */
__attribute__((noinline))
static void accel_node(mnode_t* node) {
    if (node->contents == CONTENTS_SOLID) return;
    if (node->visframe != r_visframecount) return;
    if (accel_node_culled(node)) return;   /* frustum cull this subtree */

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
        accel_surface(cl.worldmodel, surf, &g_mvp);
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

    /* water/lava turbulence phase (Quake: SPEED=20, CYCLE=128, AMP=8 texels);
     * advanced by cl.time so CRON_POLY_TURB surfaces ripple over time. */
    cron_turb((int)(cl.time * 20.0f) & 127, 8);

    accel_setup_view();
    accel_extract_frustum();
    currententity = &cl_entities[0];   /* R_TextureAnimation reads its frame */
    accel_node(cl.worldmodel->nodes);

    /* entities: alias (monsters/items/gibs), brush models (doors/platforms),
     * sprites (explosions/flames). Each helper ignores the wrong model type. */
    for (int i = 0; i < cl_numvisedicts; i++) {
        entity_t* e = cl_visedicts[i];
        if (!e->model) continue;
        if (e->model->type == mod_alias)       accel_alias(e);
        else if (e->model->type == mod_brush)  accel_brush(e);
        else if (e->model->type == mod_sprite) accel_sprite(e);
    }

    /* particles (blood, sparks, explosions) */
    accel_particles();

    /* the weapon viewmodel, drawn last (on top). Depth-hack like GLQuake: clear
     * the z-buffer first so the gun draws over the whole world and is never
     * clipped by a nearby wall (it still self-occludes against the fresh
     * buffer). Mirrors the software renderer's 3x ziscale for cl.viewent. */
    if (r_drawviewmodel.value && cl.viewent.model && !(cl.items & IT_INVISIBILITY)
        && cl.stats[STAT_HEALTH] > 0) {
        cron_zclear(0x7FFFFFFF);
        accel_alias(&cl.viewent);
    }

    cron_clip_reset();   /* restore full-screen clip for the HUD */
    cron_zbuf(0);

    /* underwater screen warp (the software path's D_WarpScreen; we forced
     * r_dowarp off so the view stays full-size, then warp it here ourselves).
     * Gated on the view leaf being in a liquid + the r_waterwarp cvar. */
    {
        extern mleaf_t* r_viewleaf;
        extern cvar_t   r_waterwarp;
        if (r_waterwarp.value && r_viewleaf && r_viewleaf->contents <= CONTENTS_WATER)
            accel_warp_view();
    }

    /* persistent on-screen indicator of the active renderer (top-right of the
     * view, clear of the top-left pickup/notify messages) */
    Draw_String(g_vx + g_vw - 52, g_vy + 4, "GPU 3D");
}
