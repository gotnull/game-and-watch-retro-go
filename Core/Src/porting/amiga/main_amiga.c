/*
 * main_amiga.c - the Amiga 500 core's host, slice 1 of 4.
 *
 * This slice is deliberately a stub: it exists to prove the launch path in
 * both directions - menu selects the Kickstart entry, this runs with the
 * hardware in the state retro-go leaves it, and quitting resets back into the
 * menu (retro-go "quits" every core via NVIC_SystemReset, so there is no
 * display state to restore on the way out - only the way in matters).
 *
 * What it does: paints the framebuffer Amiga-blue, draws the tab artwork, and
 * waits for PAUSE/SET to leave. The emulated machine itself arrives in slice
 * 3, executing in place from external flash the way the SNES ports do; the
 * plan and the memory map are in the gandw repo's RETRO-GO-CORE.md.
 */

#include "odroid_system.h"

#include "main_amiga.h"
#include "appid.h"
#include "gw_lcd.h"
#include "gw_buttons.h"
#include "rom_manager.h"
#include "bitmaps.h"
#include "main.h"

void app_main_amiga(uint8_t load_state, uint8_t start_paused, uint8_t save_slot)
{
    (void)load_state;
    (void)start_paused;
    (void)save_slot;

    odroid_system_init(APPID_AMIGA, 48000);

    /* Amiga chrome blue, the only correct colour for a placeholder. */
    const uint16_t blue = ((0x2A >> 3) << 11) | ((0x55 >> 2) << 5) | (0xAA >> 3);

    for (int frame = 0; frame < 2; frame++) {
        uint16_t *fb = lcd_get_active_buffer();
        for (int i = 0; i < 320 * 240; i++) {
            fb[i] = blue;
        }
        odroid_overlay_draw_logo((320 - header_amiga.width) / 2, 80,
                                 &header_amiga, 0xFFFF);
        lcd_swap();
    }

    for (;;) {
        wdog_refresh();
        uint32_t buttons = buttons_get();
        if (buttons & B_PAUSE) {
            /* Back to the menu the same way every core goes: a reset. */
            odroid_system_switch_app(0);
        }
        HAL_Delay(20);
    }
}
