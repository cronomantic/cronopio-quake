/* SDL_stdinc.h shim — Chocolate Quake's common/memory/menu code calls SDL's
 * libc wrappers (SDL_strX / SDL_memX / SDL_min / SDL_clamp). We don't link SDL;
 * map them to the SDK libc. Providing this on the include path lets the kept
 * sources compile unmodified. */
#ifndef CRONOPIO_QUAKE_COMPAT_SDL_STDINC_H
#define CRONOPIO_QUAKE_COMPAT_SDL_STDINC_H

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* SDL's fixed-width integer aliases — common.h builds byte/u8/i32/... on these. */
typedef uint8_t  Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;
typedef uint64_t Uint64;
typedef int8_t   Sint8;
typedef int16_t  Sint16;
typedef int32_t  Sint32;
typedef int64_t  Sint64;

#define SDL_malloc  malloc
#define SDL_calloc  calloc
#define SDL_free    free
#define SDL_realloc realloc

#define SDL_memset  memset
#define SDL_memcpy  memcpy
#define SDL_memmove memmove
#define SDL_memcmp  memcmp

#define SDL_strlen   strlen
#define SDL_strcmp   strcmp
#define SDL_strncmp  strncmp
#define SDL_strchr   strchr
#define SDL_strrchr  strrchr
#define SDL_strstr   strstr
#define SDL_strtol   strtol
#define SDL_strncasecmp strncasecmp

#ifndef SDL_min
#define SDL_min(a, b)       (((a) < (b)) ? (a) : (b))
#endif
#ifndef SDL_max
#define SDL_max(a, b)       (((a) > (b)) ? (a) : (b))
#endif
#ifndef SDL_clamp
#define SDL_clamp(x, a, b)  (((x) < (a)) ? (a) : (((x) > (b)) ? (b) : (x)))
#endif

#endif
