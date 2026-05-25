/* snd_cron.c — Cronopio sound seam for Quake (replaces sound/src/*.c).
 *
 * M0/M1: silent stub of the public S_ API the engine calls (gathered from the
 * kept subsystems). Real SFX via cron_pcm and music come in M4; this just lets
 * the engine boot and run with no audio. */

#include "quakedef.h"
#include "sound.h"

void S_Init(void)     { }
void S_Shutdown(void) { }

void S_StartSound(i32 entnum, i32 entchannel, sfx_t* sfx, vec3_t origin,
                  float fvol, float attenuation) {
    (void)entnum; (void)entchannel; (void)sfx; (void)origin;
    (void)fvol; (void)attenuation;
}

void S_StaticSound(sfx_t* sfx, vec3_t origin, float vol, float attenuation) {
    (void)sfx; (void)origin; (void)vol; (void)attenuation;
}

void S_StopSound(i32 entnum, i32 entchannel) {
    (void)entnum; (void)entchannel;
}

void S_StopAllSounds(qboolean clear) { (void)clear; }
void S_ClearBuffer(void) { }

void S_Update(vec3_t origin, vec3_t v_forward, vec3_t v_right, vec3_t v_up) {
    (void)origin; (void)v_forward; (void)v_right; (void)v_up;
}

void S_ExtraUpdate(void) { }

sfx_t* S_PrecacheSound(char* sample) { (void)sample; return NULL; }
void   S_TouchSound(char* sample)    { (void)sample; }
void   S_BeginPrecaching(void)       { }
void   S_EndPrecaching(void)         { }
void   S_LocalSound(char* s)         { (void)s; }
