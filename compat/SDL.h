/* Minimal SDL.h shim for the Cronopio Quake port.
 *
 * Real SDL is not linked. Only host.c (kept) touches the SDL subsystem-init
 * API; the actual SDL-using subsystems (sys/vid/in/snd) are replaced by our
 * src/*_cron.c. This provides just enough for host.c to compile + link: the
 * init calls become no-ops on the single-threaded, single-surface VM. */
#ifndef CRONOPIO_QUAKE_COMPAT_SDL_H
#define CRONOPIO_QUAKE_COMPAT_SDL_H

#include <stdint.h>
#include "SDL_stdinc.h"
#include "SDL_events.h"

#define SDL_INIT_TIMER          0x00000001u
#define SDL_INIT_AUDIO          0x00000010u
#define SDL_INIT_VIDEO          0x00000020u
#define SDL_INIT_JOYSTICK       0x00000200u
#define SDL_INIT_GAMECONTROLLER 0x00002000u
#define SDL_INIT_EVENTS         0x00004000u
#define SDL_INIT_EVERYTHING     0x0000FFFFu

static inline int  SDL_Init(uint32_t flags)          { (void)flags; return 0; }
static inline int  SDL_InitSubSystem(uint32_t flags) { (void)flags; return 0; }
static inline void SDL_QuitSubSystem(uint32_t flags) { (void)flags; }
static inline void SDL_Quit(void)                    { }
static inline const char* SDL_GetError(void)         { return ""; }

#endif
