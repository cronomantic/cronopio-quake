/* sys_cron.c — Cronopio implementation of Quake's non-portable Sys_ layer
 * (replaces chocolate-quake's sys/src/sys.c).
 *
 * File IO is identical to upstream: it goes through the SDK libc's stdio, which
 * is a RAM filesystem backed by the cartridge save region (the same mechanism
 * the DOOM port used for savegames). Time comes from the host virtual clock
 * (cron_time_ms). Console output goes to cron_log. Input events are pulled from
 * the 12-button pad (see in_cron.c) rather than SDL. */

#include "quakedef.h"
#include "host.h"
#include "sys.h"
#include "common.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <cronopio.h>

qboolean isDedicated;

/* in_cron.c: drain the pad into Key_Event edges (replaces SDL_PollEvent). */
extern void IN_Cron_Poll(void);

/*
=============================================================================
FILE IO  (verbatim port — stdio over the SDK libc RAM filesystem)
=============================================================================
*/

#define MAX_HANDLES 32
static FILE* sys_handles[MAX_HANDLES];

static i32 findhandle(void) {
    for (i32 i = 1; i < MAX_HANDLES; i++)
        if (!sys_handles[i])
            return i;
    Sys_Error("out of handles");
    return -1;
}

static i32 filelength(FILE* f) {
    i32 pos = ftell(f);
    fseek(f, 0, SEEK_END);
    i32 end = ftell(f);
    fseek(f, pos, SEEK_SET);
    return end;
}

i32 Sys_FileOpenRead(char* path, i32* hndl) {
    i32 i = findhandle();
    FILE* f = fopen(path, "rb");
    if (!f) {
        *hndl = -1;
        return -1;
    }
    sys_handles[i] = f;
    *hndl = i;
    return filelength(f);
}

i32 Sys_FileOpenWrite(char* path) {
    i32 i = findhandle();
    FILE* f = fopen(path, "wb");
    if (!f)
        Sys_Error("Error opening %s", path);
    sys_handles[i] = f;
    return i;
}

void Sys_FileClose(i32 handle) {
    fclose(sys_handles[handle]);
    sys_handles[handle] = NULL;
}

void Sys_FileSeek(i32 handle, i32 position) {
    fseek(sys_handles[handle], position, SEEK_SET);
}

size_t Sys_FileRead(i32 handle, void* dest, i32 count) {
    return fread(dest, 1, count, sys_handles[handle]);
}

size_t Sys_FileWrite(i32 handle, void* data, i32 count) {
    return fwrite(data, 1, count, sys_handles[handle]);
}

i32 Sys_FileTime(char* path) {
    FILE* f = fopen(path, "rb");
    if (f) {
        fclose(f);
        return 1;
    }
    return -1;
}

void Sys_mkdir(char* path) {
    (void)path;   /* the RAM-FS is flat; directories are implicit */
}

/*
=============================================================================
SYSTEM IO
=============================================================================
*/

static void cron_puts(const char* s) {
    cron_log(s, (int32_t)strlen(s));
}

void Sys_Error(char* error, ...) {
    va_list argptr;
    char string[1024];

    va_start(argptr, error);
    vsnprintf(string, sizeof(string), error, argptr);
    va_end(argptr);

    cron_puts("\nQuake Error: ");
    cron_puts(string);
    cron_puts("\n");

    Host_Shutdown();
    cron_exit(1);
    for (;;) { }   /* unreachable; keeps the compiler happy */
}

void Sys_Printf(char* fmt, ...) {
    va_list argptr;
    char string[1024];

    va_start(argptr, fmt);
    vsnprintf(string, sizeof(string), fmt, argptr);
    va_end(argptr);

    cron_puts(string);
}

void Sys_Quit(void) {
    Host_Shutdown();
    cron_exit(0);
    for (;;) { }
}

double Sys_FloatTime(void) {
    return (double)cron_time_ms() / 1000.0;
}

char* Sys_ConsoleInput(void) {
    return NULL;
}

void Sys_SendKeyEvents(void) {
    IN_Cron_Poll();
}

void Sys_HighFPPrecision(void) { }
void Sys_LowFPPrecision(void)  { }

/*
=============================================================================
INIT
=============================================================================
*/

/* The hunk/zone size. Vanilla Quake ran in 8-16 MB; the surface cache scales
 * with resolution and at 320x240 is small. Keep it well under the cart heap
 * reserve (build_quake.sh: 96M). */
#define CRON_QUAKE_MEMORY (32 * 1024 * 1024)

quakeparms_t* Sys_Init(i32 argc, char* argv[]) {
    static quakeparms_t parms;

    parms.memsize = CRON_QUAKE_MEMORY;
    parms.membase = Q_malloc(parms.memsize);
    parms.basedir = ".";
    parms.cachedir = NULL;

    COM_InitArgv(argc, argv);
    parms.argc = com_argc;
    parms.argv = com_argv;

    return &parms;
}
