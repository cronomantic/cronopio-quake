/* vid_cron.c — Cronopio video seam for Quake (replaces video/src/vid_*.c).
 *
 * Quake's software renderer draws 8-bit palette indices into vid.buffer and
 * keeps its z-buffer + surface cache in the high hunk. We give it a 320x240
 * paletted buffer (the Cronopio screen size), allocate the z/surfcache exactly
 * like upstream's vid_buffers.c, and VID_Update blits vid.buffer -> CRON_FB
 * and pushes the palette to CRON_PAL. No SDL, no window, single page. */

#include "quakedef.h"
#include "vid.h"
#include "d_local.h"
#include "render.h"
#include "host.h"
#include "zone.h"
#include "sys.h"

#include <string.h>

#include <cronopio.h>

viddef_t vid;   // global video state

#define VID_W CRON_SCREEN_W   // 320
#define VID_H CRON_SCREEN_H   // 240

static byte     vid_screen[VID_W * VID_H];   // the paletted draw buffer
static byte     vid_palette[256 * 3];        // last palette set by the engine
static qboolean palette_dirty;
static qboolean vid_initialized;
static i32      vid_surfcachesize;

/* ---- palette ----------------------------------------------------------- */

void VID_SetPalette(const byte* palette) {
    memcpy(vid_palette, palette, sizeof(vid_palette));
    palette_dirty = true;
}

void VID_ShiftPalette(const byte* palette) {
    VID_SetPalette(palette);
}

static void VID_FlushPalette(void) {
    for (i32 i = 0; i < 256; i++) {
        uint32_t r = vid_palette[i * 3 + 0];
        uint32_t g = vid_palette[i * 3 + 1];
        uint32_t b = vid_palette[i * 3 + 2];
        CRON_PAL[i] = (r << 16) | (g << 8) | b;
    }
    palette_dirty = false;
}

/* ---- buffers (z-buffer + surface cache in the high hunk) --------------- */

static void VID_AllocBuffers(void) {
    vid_surfcachesize = D_SurfaceCacheForRes(vid.width, vid.height);

    i32 chunk = vid.width * vid.height * (i32)sizeof(*d_pzbuffer);
    chunk += vid_surfcachesize;

    d_pzbuffer = Hunk_HighAllocName(chunk, "video");
    if (!d_pzbuffer) {
        Sys_Error("Not enough memory for video mode\n");
    }

    byte* base = (byte*)d_pzbuffer;
    size_t cache_offset = vid.width * vid.height * sizeof(*d_pzbuffer);
    D_InitCaches(&base[cache_offset], vid_surfcachesize);
}

/* ---- init / shutdown --------------------------------------------------- */

void VID_Init(const byte* palette) {
    vid.width  = VID_W;
    vid.height = VID_H;
    vid.aspect = ((float)vid.height / (float)vid.width) * (320.0f / 240.0f);
    vid.numpages = 1;
    vid.recalc_refdef = true;
    vid.colormap = host_colormap;
    vid.buffer = vid_screen;

    VID_AllocBuffers();
    VID_SetPalette(palette);
    VID_FlushPalette();

    vid_initialized = true;
}

void VID_Shutdown(void) {
    vid_initialized = false;
}

/* ---- per-frame present ------------------------------------------------- */

void VID_Update(vrect_t* rects) {
    (void)rects;   // we always flush the whole frame
    if (palette_dirty) {
        VID_FlushPalette();
    }
    /* vid.buffer is 320x240 indices; CRON_FB is the same size. */
    volatile uint8_t* fb = CRON_FB;
    const byte* src = vid.buffer;
    for (i32 i = 0; i < VID_W * VID_H; i++) {
        fb[i] = src[i];
    }
    cron_present();
}

void VID_SetMode(i32 modenum) {
    (void)modenum;   // single fixed mode on the console
}

/* ---- direct-rect (loading disk icon) — no-op on Cronopio --------------- */

void D_BeginDirectRect(i32 x, i32 y, byte* pbitmap, i32 width, i32 height) {
    (void)x; (void)y; (void)pbitmap; (void)width; (void)height;
}

void D_EndDirectRect(i32 x, i32 y, i32 width, i32 height) {
    (void)x; (void)y; (void)width; (void)height;
}

/* ---- misc seam no-ops the engine may call ------------------------------ */

void VID_LockBuffer(void)   { }
void VID_UnlockBuffer(void) { }
void VID_HandlePause(qboolean pause) { (void)pause; }
void VID_MenuDraw(void) { }
void VID_MenuKey(i32 key) { (void)key; }
qboolean VID_IsFullscreenMode(void) { return true; }
qboolean VID_IsWindowedMode(void)   { return false; }
qboolean VID_WindowedMouse(void)    { return false; }
void VID_ToggleMouseGrab(void)      { }
