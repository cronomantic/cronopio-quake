/* Minimal SDL_events.h shim — input.h declares IN_KeyboardEvent(SDL_Event*),
 * IN_MouseEvent, IN_GamepadEvent (whose real definitions live in the replaced
 * in_*.c). Kept code that includes input.h (keys.c, menu.c) only needs the
 * SDL_Event type name to exist; our in_cron.c never receives one (the pad is
 * polled directly). */
#ifndef CRONOPIO_QUAKE_COMPAT_SDL_EVENTS_H
#define CRONOPIO_QUAKE_COMPAT_SDL_EVENTS_H

#include <stdint.h>

typedef union SDL_Event {
    uint32_t type;
    uint8_t  padding[64];
} SDL_Event;

#endif
