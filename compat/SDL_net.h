/* Minimal SDL_net.h shim — net.h includes it for the IPaddress type stored in
 * hostcache_t. The cart only runs the loopback driver (single-player), so the
 * real SDL_net API is never called; we just need the type to exist. */
#ifndef CRONOPIO_QUAKE_COMPAT_SDL_NET_H
#define CRONOPIO_QUAKE_COMPAT_SDL_NET_H

#include <stdint.h>

typedef struct {
    uint32_t host;   /* 32-bit IPv4 host address */
    uint16_t port;   /* 16-bit protocol port */
} IPaddress;

#endif
