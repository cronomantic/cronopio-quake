/* in_cron.c — Cronopio input seam for Quake (replaces input/src/in_*.c).
 *
 * Quake reads input two ways: digital key events (Key_Event, bound to commands
 * like +forward/+attack via the config) and analog deltas (IN_Move, for mouse
 * look). The Cronopio console exposes ONE abstract 12-button pad (the host maps
 * physical keyboard/controller onto it). We translate the pad into Quake key
 * edges; there is no mouse, so IN_Move is empty.
 *
 * IN_Cron_Poll() is called from Sys_SendKeyEvents() once per frame: it reads
 * the pad and posts one Key_Event edge per Quake key that changed. The
 * SDL-specific IN_*Event entry points exist only to satisfy input.h and are
 * never called on the cartridge. */

#include "quakedef.h"
#include "input.h"
#include "keys.h"
#include "client.h"

#include <string.h>

#include <cronopio.h>

#ifndef CRON_BTN_START
#define CRON_BTN_START (1u << 10)
#endif
#ifndef CRON_BTN_SELECT
#define CRON_BTN_SELECT (1u << 11)
#endif

void IN_Init(void)     { }
void IN_Shutdown(void) { }

/* These take SDL_Event* (shimmed type); never invoked on the cart. */
void IN_KeyboardEvent(const SDL_Event* e) { (void)e; }
void IN_MouseEvent(const SDL_Event* e)    { (void)e; }
void IN_GamepadEvent(const SDL_Event* e)  { (void)e; }

void IN_Move(usercmd_t* cmd) { (void)cmd; }   /* no analog look */

void IN_DeactivateMouse(void) { }
void IN_ActivateMouse(void)   { }
void IN_ShowMouse(void)       { }
void IN_HideMouse(void)       { }
void IN_ClearStates(void)     { }

/* ---- pad -> Quake keys ------------------------------------------------- */

typedef struct {
    uint32_t bit;
    int      key;   /* Quake key in play */
} padmap_t;

/* In-game: the d-pad drives movement/turn (vanilla default.cfg binds the
 * arrows to +forward/+back/+left/+right), A=attack, B=jump, START=menu,
 * SELECT=console. Weapon/strafe refinement comes in M3. */
static const padmap_t map_game[] = {
    {CRON_BTN_UP,     K_UPARROW},
    {CRON_BTN_DOWN,   K_DOWNARROW},
    {CRON_BTN_LEFT,   K_LEFTARROW},
    {CRON_BTN_RIGHT,  K_RIGHTARROW},
    {CRON_BTN_A,      K_CTRL},      /* +attack */
    {CRON_BTN_B,      K_SPACE},     /* +jump   */
    {CRON_BTN_X,      '/'},         /* (unbound yet) */
    {CRON_BTN_Y,      '.'},
    {CRON_BTN_L,      ','},
    {CRON_BTN_R,      '/'},
    {CRON_BTN_START,  K_ESCAPE},
    {CRON_BTN_SELECT, '`'},         /* toggle console */
};

/* Menu/console: A confirms, B/START cancel; the d-pad navigates; SELECT still
 * toggles the console. */
static const padmap_t map_menu[] = {
    {CRON_BTN_UP,     K_UPARROW},
    {CRON_BTN_DOWN,   K_DOWNARROW},
    {CRON_BTN_LEFT,   K_LEFTARROW},
    {CRON_BTN_RIGHT,  K_RIGHTARROW},
    {CRON_BTN_A,      K_ENTER},
    {CRON_BTN_B,      K_ESCAPE},
    {CRON_BTN_START,  K_ESCAPE},
    {CRON_BTN_SELECT, '`'},
};

#define N_GAME ((int)(sizeof(map_game) / sizeof(map_game[0])))
#define N_MENU ((int)(sizeof(map_menu) / sizeof(map_menu[0])))

static uint8_t key_now[256];
static uint8_t key_prev[256];

static void mark(int key) {
    if (key > 0 && key < 256) {
        key_now[key] = 1;
    }
}

void IN_Cron_Poll(void) {
    uint32_t pad = cron_pad(0);

    memset(key_now, 0, sizeof(key_now));

    const padmap_t* map = (key_dest == key_game) ? map_game : map_menu;
    const int       n   = (key_dest == key_game) ? N_GAME   : N_MENU;
    for (int i = 0; i < n; i++) {
        if (pad & map[i].bit) {
            mark(map[i].key);
        }
    }

    for (int k = 1; k < 256; k++) {
        if (key_now[k] != key_prev[k]) {
            Key_Event(k, key_now[k] != 0);
            key_prev[k] = key_now[k];
        }
    }
}
