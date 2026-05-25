/* engine_cron.c — Cronopio cartridge entry for Quake (replaces main.c).
 *
 * Quake's loop is Sys_Init -> Host_Init -> repeat Host_Frame(dt). On Cronopio
 * the host owns the loop, so we run init once in main() and hand one
 * Host_Frame per frame to cron_set_frame(). dt comes from the host virtual
 * clock via Sys_FloatTime(). */

#include <cronopio.h>

#include "quakedef.h"
#include "host.h"
#include "sys.h"

/* The host expects the cart to define storage for these; cron_resolve_video()
 * fills them with the real heap pointers. */
volatile uint8_t  *CRON_FB  = 0;
volatile uint32_t *CRON_PAL = 0;

static double g_old_time;

static void quake_frame(void) {
    double new_time = Sys_FloatTime();
    double dt = new_time - g_old_time;
    if (dt > 0.1) {
        dt = 0.1;   /* clamp huge stalls, like the original loop */
    }
    Host_Frame((float)dt);
    g_old_time = new_time;
}

/* We own argc/argv (no command line on a cartridge). */
static char* fake_argv[] = {(char*)"quake"};

int main(void) {
    if (cron_resolve_video() != 0) {
        const char msg[] = "quake: fb/pal regions missing\n";
        cron_log(msg, (int32_t)(sizeof(msg) - 1));
        return 1;
    }

    /* Simple boot splash; the real loading bar can come later (see DOOM's
     * platform.c plat_boot_*). */
    cron_palette_set(0, 0x000000);
    cron_palette_set(1, 0xC8C8C8);
    cron_cls(0);
    cron_text("CRONOPIO-QUAKE", 14, (CRON_SCREEN_W - 14 * 8) / 2, 112, 1);
    cron_present();

    /* Serve the baked PAK (cron --rom blob) as ./id1/pak0.pak — the exact path
     * COM_AddGameDirectory builds from basedir="." + GAMENAME "id1". Quake then
     * opens it through normal fopen and reads lumps straight from ROM. */
    cron_rom_mount("./id1/pak0.pak");

    quakeparms_t* parms = Sys_Init(1, fake_argv);
    Host_Init(parms);

    g_old_time = Sys_FloatTime();
    cron_set_frame(quake_frame);
    return 0;
}
