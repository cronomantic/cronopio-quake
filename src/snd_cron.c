/* snd_cron.c — Cronopio sound seam for Quake (replaces sound/src/*.c).
 *
 * M0/M1: silent stub of the public S_ API the engine calls (gathered from the
 * kept subsystems). Real SFX via cron_pcm and music come in M4; this just lets
 * the engine boot and run with no audio. */

#include "quakedef.h"
#include "sound.h"

/* Volume cvars normally defined in snd_dma.c (stubbed out). Kept code reads
 * these globals (menu sliders) and the named cvars, so define + register them
 * even though playback is silent. */
cvar_t bgmvolume = {"bgmvolume", "1", true};
cvar_t sfxvolume = {"volume", "0.7", true};

void S_Init(void) {
    Cvar_RegisterVariable(&bgmvolume);
    Cvar_RegisterVariable(&sfxvolume);
}
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

/* Background music (CD/track) — silent stubs (real music is M4). */
i32  BGMusic_Init(void)                          { return 1; }
void BGMusic_Play(byte track, qboolean looping)  { (void)track; (void)looping; }
void BGMusic_Stop(void)                          { }
void BGMusic_Pause(void)                         { }
void BGMusic_Resume(void)                        { }
void BGMusic_Shutdown(void)                      { }
void BGMusic_Update(void)                        { }

sfx_t* S_PrecacheSound(char* sample) { (void)sample; return NULL; }
void   S_TouchSound(char* sample)    { (void)sample; }
void   S_BeginPrecaching(void)       { }
void   S_EndPrecaching(void)         { }
void   S_LocalSound(char* s)         { (void)s; }
