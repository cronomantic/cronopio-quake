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
extern cvar_t r_drawviewmodel;      /* r_main.c */

#define ACCEL_NEAR   4.0f
#define ACCEL_FAR    8192.0f
#define MAXSURFVERTS 32             /* polygon verts per surface (fan source) */

static cron_mat4   g_mvp;
static int         g_vx, g_vy, g_vw, g_vh;   /* viewport rect */
static vec3_t      g_vright, g_vup;          /* view basis (for camera-facing sprites) */

static int32_t     g_zbuffer[CRON_SCREEN_W * CRON_SCREEN_H];
static byte        g_lmgrid[18 * 18];        /* per-surface light rows (0..63) */
static cron_vert_t g_batch[6];               /* one (clipped) triangle = up to 6 */

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
static void accel_build_lightmap(model_t* m, msurface_t* s, int smax, int tmax) {
    int size = smax * tmax;
    byte* lightmap = s->samples;

    if (!m->lightdata || !lightmap) {
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

/* Emit one surface (world or brush model) as a triangle fan, transformed by
 * `mvp` (proj*view for the world; proj*view*model for a brush entity). */
__attribute__((noinline))
static void accel_surface(model_t* m, msurface_t* s, const cron_mat4* mvp) {
    texture_t* tex = s->texinfo->texture;
    if (!tex) return;

    int is_sky  = s->flags & SURF_DRAWSKY;
    int is_turb = s->flags & SURF_DRAWTURB;

    cron_image(0, (const uint8_t*)tex + tex->offsets[0], (int)tex->width, (int)tex->height);

    int mode;
    if (is_sky || is_turb) {
        /* sky and water/lava are fullbright (no lightmap); identity cmap. */
        cron_cmap(0);
        mode = CRON_POLY_TEX | CRON_POLY_PERSP | CRON_POLY_ZTEST;
    } else {
        int smax = (s->extents[0] >> 4) + 1;
        int tmax = (s->extents[1] >> 4) + 1;
        if (smax > 18) smax = 18;
        if (tmax > 18) tmax = 18;
        accel_build_lightmap(m, s, smax, tmax);
        cron_lightmap(g_lmgrid, smax, tmax);
        mode = CRON_POLY_TEX | CRON_POLY_LIGHTMAP | CRON_POLY_PERSP | CRON_POLY_ZTEST;
    }

    /* sky scroll (GLQuake-style): texcoords from the view direction. */
    float skyscroll = (float)cl.time * 8.0f;
    skyscroll -= (float)((int)(skyscroll * (1.0f/128.0f))) * 128.0f;

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
            A[i].u = skyscroll + d0 * len;
            A[i].v = skyscroll + d1 * len;
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

    aliashdr_t* ph   = (aliashdr_t*)Mod_Extradata(model);
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

    const int mode = CRON_POLY_TEX | CRON_POLY_PERSP | CRON_POLY_ZTEST;

    for (int t = 0; t < pmdl->numtris; t++) {
        mtriangle_t* tri = &tris[t];
        cron_cvert v[3], clipped[6];
        for (int k = 0; k < 3; k++) {
            int vi = tri->vertindex[k];
            trivertx_t* tv = &verts[vi];
            cron_vec3 p = cron_v3((float)tv->v[0], (float)tv->v[1], (float)tv->v[2]);
            cron_mat_point(&v[k].pos, &mvpm, p);
            float s = (float)stv[vi].s;
            float tt = (float)stv[vi].t;
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
    msprite_t* ps = (msprite_t*)Mod_Extradata(model);

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

    accel_setup_view();
    accel_node(cl.worldmodel->nodes);

    /* entities: alias (monsters/items/gibs) and brush models (doors/platforms).
     * Each helper ignores models of the wrong type; sprites are a later phase. */
    for (int i = 0; i < cl_numvisedicts; i++) {
        entity_t* e = cl_visedicts[i];
        if (!e->model) continue;
        if (e->model->type == mod_alias)       accel_alias(e);
        else if (e->model->type == mod_brush)  accel_brush(e);
        else if (e->model->type == mod_sprite) accel_sprite(e);
    }

    /* particles (blood, sparks, explosions) */
    accel_particles();

    /* the weapon viewmodel, drawn last (on top) */
    if (r_drawviewmodel.value && cl.viewent.model && !(cl.items & IT_INVISIBILITY)
        && cl.stats[STAT_HEALTH] > 0)
        accel_alias(&cl.viewent);

    cron_clip_reset();   /* restore full-screen clip for the HUD */
    cron_zbuf(0);
}
