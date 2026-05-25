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

/* Opaque socket/packet types. net_main.c (kept) includes net_socket.h, whose
 * declarations mention these; the real SDLNet_* functions live only in the
 * datagram/UDP/socket .c files we don't build (loopback-only). Stub types are
 * enough for the kept code to compile. */
typedef void* UDPsocket;
typedef void* TCPsocket;
typedef void* SDLNet_SocketSet;

typedef struct {
    int       channel;
    uint8_t*  data;
    int       len, maxlen, status;
    IPaddress address;
} UDPpacket;

#endif
