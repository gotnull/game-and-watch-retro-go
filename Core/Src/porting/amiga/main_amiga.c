/*
 * main_amiga.c - the Amiga 500 core's host inside retro-go.
 *
 * The emulated machine is rusty-nail's fcamiga, reached through the small C
 * API of the gandw repo's fcamiga-gw staticlib. This file owns everything
 * hardware: the LTDC switch to indexed colour, the blit, input, and the exit.
 * Nothing in the machine knows it is inside retro-go.
 *
 * DISPLAY
 * -------
 * The launcher hands over an RGB565 LTDC scanning .lcd1/.lcd2. This core's
 * Chip RAM sits ON TOP of those buffers, so the very first thing done here is
 * repointing the layer at this core's own L8 surface - before the machine is
 * initialised, because initialising it scribbles over what the panel is
 * showing. The layer switch is one register write per field plus a reload;
 * the CLUT does the colour, fed per frame from the machine's own palette.
 *
 * There is no display restore on exit. retro-go quits every core with
 * NVIC_SystemReset and the launcher brings the LTDC up from scratch.
 *
 * WHAT LAUNCHES WHAT
 * ------------------
 * The Kickstart .rom entry boots the bare machine - the insert-disk screen.
 * An .adf entry boots the machine with that disk in the drive, finding the
 * Kickstart among the system's .rom files. Both execute in place from
 * external flash; the ROM must be pre-swapped (tools/kickstart_swap.py).
 */

#include <string.h>
#include "odroid_system.h"

#include "main_amiga.h"
#include "appid.h"
#include "gw_lcd.h"
#include "gw_audio.h"
#include "gw_buttons.h"
#include "main.h"
#include "rom_manager.h"
#include "bitmaps.h"
#include "stm32h7xx.h"

extern const rom_system_t amiga_system;

/* ---------------------------------------------------------------------------
 * Newlib stubs the staticlib's embedded libc objects reference. Nothing here
 * should ever be reached - no console, no filesystem - and each REFUSES
 * rather than fakes success: an _sbrk handing out addresses would scribble
 * over whatever bank they landed in.
 * ------------------------------------------------------------------------ */
int   _close(int fd)                        { (void)fd; return -1; }
int   _fstat(int fd, void *st)              { (void)fd; (void)st; return -1; }
int   _isatty(int fd)                       { (void)fd; return 0; }
int   _lseek(int fd, int off, int whence)   { (void)fd; (void)off; (void)whence; return -1; }
int   _read(int fd, char *buf, int len)     { (void)fd; (void)buf; (void)len; return -1; }
/* _sbrk: retro-go already provides one (gw_alloc.c). */

/* --- The machine (fcamiga-gw staticlib) --- */
int             amiga_init(const uint8_t *rom, uint32_t rom_len);
int             amiga_insert_disk(const uint8_t *adf, uint32_t len);
void            amiga_run_frame(void);
const uint8_t  *amiga_capture(void);
const uint16_t *amiga_palettes(void);
uint32_t        amiga_rows(void);
uint32_t        amiga_row_stride(void);
void            amiga_set_input(uint16_t pad_mask);
void            amiga_set_mouse(int8_t dx, int8_t dy, int lmb, int rmb);
uint32_t        amiga_drain_audio(int16_t *out, uint32_t len);

/* The L8 surface the LTDC scans. Placed by the .amiga_fb linker section. */
static uint8_t amiga_framebuffer[320 * 240]
    __attribute__((section(".amiga_fb"), aligned(32)));

#define LORES_CANVAS 368

/* ---------------------------------------------------------------------------
 * LTDC: hand the panel to the machine.
 *
 * Lesson carried over from the gandw SDK, where it cost a day: LTDC layer
 * registers are shadowed - a write goes to the shadow, a READ returns the
 * active value. Two read-modify-writes of LxCR therefore do not accumulate;
 * CLUTEN and LEN must be set in ONE assignment.
 * ------------------------------------------------------------------------ */
static void ltdc_enter_indexed(void)
{
    /*
     * Silence retro-go's display machinery FIRST. Its LTDC line interrupt
     * drives an lcd_swap pipeline that reprograms CFBAR and the pixel format
     * every frame - measured on hardware: seconds after this function ran,
     * the layer read PFCR=2 (RGB565) and CFBAR=.lcd2 again, while the panel
     * showed the abandoned menu buffer. The machine was running perfectly
     * underneath; its picture just never stayed on screen for more than a
     * frame. This core paces itself on SRCR_VBR and needs no LTDC interrupt;
     * everything is restored by the exit-is-reset convention.
     */
    NVIC_DisableIRQ(LTDC_IRQn);
    NVIC_DisableIRQ(LTDC_ER_IRQn);
    LTDC->IER = 0;

    LTDC_Layer1->CFBAR  = (uint32_t)amiga_framebuffer;
    LTDC_Layer1->PFCR   = 5;                              /* L8 */
    LTDC_Layer1->CFBLR  = (320u << 16) | (320u + 3u);     /* 1 byte per pixel */
    LTDC_Layer1->CFBLNR = 240;
    LTDC_Layer1->CR     = LTDC_LxCR_CLUTEN | LTDC_LxCR_LEN;
    LTDC->SRCR = LTDC_SRCR_IMR;
}

static void load_clut(const uint16_t *row_palette)
{
    /* OCS RGB444 -> RGB888, nibble replicated so white stays white. */
    for (int i = 0; i < 32; i++) {
        const uint16_t c = row_palette[i];
        const uint32_t r = ((c >> 8) & 0xF) * 17u;
        const uint32_t g = ((c >> 4) & 0xF) * 17u;
        const uint32_t b = ((c >> 0) & 0xF) * 17u;
        LTDC_Layer1->CLUTWR = ((uint32_t)i << 24) | (r << 16) | (g << 8) | b;
    }
}

/*
 * Full-canvas view: all 368 lores columns scaled into 320 panel pixels, so
 * nothing the machine displays is ever cut off. The map holds byte offsets
 * into the 736-entry capture row (lores pixels are written twice).
 */
static uint16_t column_map[320];

static void build_column_map(void)
{
    for (int x = 0; x < 320; x++) {
        column_map[x] = (uint16_t)(((uint32_t)x * LORES_CANVAS / 320) * 2u);
    }
}

static void blit(const uint8_t *capture, uint32_t rows, uint32_t stride)
{
    const uint32_t lines = rows < 240 ? rows : 240;
    for (uint32_t y = 0; y < lines; y++) {
        const uint8_t *src = capture + y * stride;
        uint8_t *dst = amiga_framebuffer + y * 320;
        for (int x = 0; x < 320; x++) {
            dst[x] = src[column_map[x]];
        }
    }
    SCB_CleanDCache_by_Addr((uint32_t *)amiga_framebuffer, sizeof amiga_framebuffer);
}

/* Failures have no console to print to; they have 76,800 pixels. */
static void die(uint8_t colour_index, uint32_t rgb888)
{
    LTDC_Layer1->CLUTWR = ((uint32_t)colour_index << 24) | rgb888;
    for (int i = 0; i < 320 * 240; i++) {
        amiga_framebuffer[i] = colour_index;
    }
    SCB_CleanDCache_by_Addr((uint32_t *)amiga_framebuffer, sizeof amiga_framebuffer);
    for (;;) {
        wdog_refresh();
        if (buttons_get() & B_PAUSE) {
            odroid_system_switch_app(0);
        }
        HAL_Delay(20);
    }
}

void app_main_amiga(uint8_t load_state, uint8_t start_paused, uint8_t save_slot)
{
    (void)load_state;
    (void)start_paused;
    (void)save_slot;

    odroid_system_init(APPID_AMIGA, 44100);

    /*
     * Paula produces 44,118Hz stereo; the SAI runs at 44,100. The 0.04%
     * difference is far below audibility and simply lets the ring drain
     * fractionally faster than it fills. 44,118 / 50.02 frames = 882
     * samples a frame, which is exactly what each DMA half holds.
     */
    audio_clear_active_buffer();
    audio_clear_inactive_buffer();
    audio_start_playing(882);

    /*
     * Bring the staticlib's spilled statics to life. The linker parks them in
     * AHBRAM (DTCM cannot hold them alongside retro-go's own data), inside a
     * NOLOAD region retro-go's startup never touches - so the zeroing and the
     * .data copy that a C runtime would normally do happen right here, and
     * they MUST precede the first fcamiga call.
     */
    /*
     * Enable the AHB SRAM clocks FIRST. SystemInit only does this under
     * DATA_IN_D2_SRAM, which retro-go does not define - nothing else in the
     * firmware ever bulk-writes this bank from the CPU, so the clocks have
     * been off since reset and the first memset into SRAM2 bus-faulted.
     * Found via the exception frame: r0/r1/r2 were perfect and the write
     * still died; AHB2ENR read back zero.
     */
    RCC->AHB2ENR |= RCC_AHB2ENR_AHBSRAM1EN | RCC_AHB2ENR_AHBSRAM2EN;
    (void)RCC->AHB2ENR;

    /*
     * Initialise the ENTIRE bank with pure 64-bit stores before anything
     * narrower touches it. These SRAMs are ECC-protected and, with their
     * clocks off since reset, the ECC bits hold garbage. A 32-bit store is a
     * read-modify-write against that garbage and bus-faults - which is why a
     * plain memset here died at a DIFFERENT address on every run (BFAR
     * 0x30012064 one boot, 0x30017B20 the next) while SWD pokes worked.
     * Doubleword writes replace the whole ECC granule and never read.
     */
    /*
     * Zero the bank once with aligned doubleword stores. Keeps the ECC clean
     * on first touch and costs nothing measurable at 280MHz.
     */
    for (volatile uint64_t *p64 = (uint64_t *)0x30000000u;
         p64 < (uint64_t *)0x30020000u; p64++) {
        *p64 = 0;
    }

    extern uint8_t __amiga_data_start__, __amiga_data_end__, __amiga_data_load__;
    memcpy(&__amiga_data_start__, &__amiga_data_load__,
           (size_t)(&__amiga_data_end__ - &__amiga_data_start__));

    build_column_map();
    ltdc_enter_indexed();


    /*
     * Which files play which role. The launched entry is in ROM_DATA; if it
     * is a disk, the Kickstart is found among the system's .rom entries.
     */
    const uint8_t *kick = NULL;
    uint32_t kick_len = 0;
    const uint8_t *disk = NULL;

    if (ACTIVE_FILE->ext[0] == 'r') {                       /* "rom" */
        kick = ROM_DATA;
        kick_len = ROM_DATA_LENGTH;
    } else {                                                /* "adf" */
        disk = ROM_DATA;
        const retro_emulator_file_t *k =
            rom_get_ext_file_at_index(&amiga_system, "rom", 0);
        if (k == NULL) {
            die(1, 0x00FF8800);      /* orange: disk without any Kickstart */
        }
        kick = (const uint8_t *)k->address;
        kick_len = k->size;
    }

    /*
     * A pre-swapped Kickstart starts 0x1111 0x4EF9 as little-endian words.
     * An unswapped one reads 0x1111 in the WRONG half. Catching it here turns
     * "black screen" into a colour with a known meaning.
     */
    if (((const uint16_t *)kick)[0] != 0x1111) {
        die(1, 0x00FF0000);          /* red: Kickstart not byte-swapped */
    }

    if (amiga_init(kick, kick_len) != 0) {
        die(1, 0x000000FF);          /* blue: core rejected the ROM */
    }

    if (disk != NULL && amiga_insert_disk(disk, 901120) != 0) {
        die(1, 0x00FF00FF);          /* magenta: core rejected the disk */
    }

    const uint8_t  *capture  = amiga_capture();
    const uint16_t *palettes = amiga_palettes();
    const uint32_t  rows     = amiga_rows();
    const uint32_t  stride   = amiga_row_stride();

    static int16_t audio_scratch[2048] __attribute__((section(".ahb")));

    for (;;) {
        wdog_refresh();

        const uint32_t btn = buttons_get();

        /*
         * Same mapping as the standalone image: d-pad drives joystick AND
         * mouse (both ports are live on a real Amiga, no mode switch), A is
         * fire, GAME/TIME are the mouse buttons - never A or B, because a
         * requester asking for the left mouse button should not be answered
         * by the fire button. PAUSE leaves.
         */
        if (btn & B_PAUSE) {
            odroid_system_switch_app(0);
        }

        uint16_t pad = 0;
        if (btn & B_Left)  pad |= 0x0001;
        if (btn & B_Right) pad |= 0x0002;
        if (btn & B_Up)    pad |= 0x0004;
        if (btn & B_Down)  pad |= 0x0008;
        if (btn & B_A)     pad |= 0x0010;
        amiga_set_input(pad);

        const int8_t step = 4;
        int8_t dx = 0, dy = 0;
        if (btn & B_Left)  dx = -step;
        if (btn & B_Right) dx =  step;
        if (btn & B_Up)    dy = -step;
        if (btn & B_Down)  dy =  step;
        amiga_set_mouse(dx, dy, (btn & B_GAME) != 0, (btn & B_TIME) != 0);

        amiga_run_frame();

        /*
         * Paula to the speaker: drain one frame's worth of stereo, downmix
         * to mono into the DMA half the hardware is not currently playing.
         * The downmix halves each channel first - the sum of two full-scale
         * channels would clip, and this speaker is not worth clipping for.
         */
        {
            const uint32_t got = amiga_drain_audio(audio_scratch, 882 * 2);
            int16_t *out = audio_get_inactive_buffer();
            const uint32_t frames_got = got / 2;
            for (uint32_t i = 0; i < 882; i++) {
                if (i < frames_got) {
                    out[i] = (int16_t)((audio_scratch[2 * i] / 2)
                                     + (audio_scratch[2 * i + 1] / 2)) / 2;
                } else {
                    out[i] = 0;
                }
            }
        }

        load_clut(palettes);

        /*
         * Wait for vblank BEFORE blitting, then blit immediately - the same
         * ordering the standalone image settled on. One framebuffer means
         * the beam reads what the CPU writes; blitting just after vblank
         * keeps the CPU ahead of it for the whole frame, where blitting just
         * before showed as flickering bands below the image.
         */
        LTDC->SRCR = LTDC_SRCR_VBR;
        while (LTDC->SRCR & LTDC_SRCR_VBR) { }
        blit(capture, rows, stride);
    }
}
