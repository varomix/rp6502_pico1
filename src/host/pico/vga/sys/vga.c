/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "vga/main.h"
#include "vga/sys/ria.h"
#include "vga/sys/vga.h"
#include "vga/sys/com.h"
#include "vga/sys/cap.h"
#include <stdio.h>
#include <hardware/timer.h>
#include "core/vga/prog.h"
#include "core/sys/xram.h"
#include "core/term/term.h"
#include "vga/scanvideo/scanvideo.h"
#include <pico/stdlib.h>
#include <pico/multicore.h>
#include <hardware/clocks.h>
#if PICO_RP2040
#include <hardware/vreg.h>
#endif
#include <string.h>

#pragma GCC push_options
#pragma GCC optimize("O3")

static mutex_t vga_scanline_mutex;
static volatile bool vga_rendering[2];
static volatile uint16_t vga_vsync_frame_fired;
static vga_display_t vga_display_selected;
static vga_canvas_t vga_canvas_selected;
static volatile scanvideo_view_t const *vga_view_current;
static scanvideo_view_t const *vga_view_selected;
static volatile bool vga_view_switching;

static const scanvideo_timing_t vga_timing_640x480_60_cea = {
    .clock_freq = 25200000,

    .h_active = 640,
    .v_active = 480,

    .h_front_porch = 16,
    .h_pulse = 96,
    .h_total = 800,
    .h_sync_polarity = 1,

    .v_front_porch = 10,
    .v_pulse = 2,
    .v_total = 525,
    .v_sync_polarity = 1};

static const scanvideo_timing_t vga_timing_1280x1024_60_dmt = {
    // half clock rate, effective 2 x_scale
    .clock_freq = 54000000,

    .h_active = 640,
    .v_active = 1024,

    .h_front_porch = 24,
    .h_pulse = 56,
    .h_total = 844,
    .h_sync_polarity = 0,

    .v_front_porch = 1,
    .v_pulse = 3,
    .v_total = 1066,
    .v_sync_polarity = 1};

static const scanvideo_timing_t vga_timing_1280x720_60_cea = {
    // half clock rate, effective 2 x_scale
    .clock_freq = 37125000,

    .h_active = 640,
    .v_active = 720,

    .h_front_porch = 55,
    .h_pulse = 20,
    .h_total = 825,
    .h_sync_polarity = 1,

    .v_front_porch = 5,
    .v_pulse = 5,
    .v_total = 750,
    .v_sync_polarity = 1};

static const scanvideo_view_t vga_view_320x240 = {
    .default_timing = &vga_timing_640x480_60_cea,
    .width = 320,
    .height = 240,
    .x_scale = 2,
    .y_scale = 2,
    .y_offset = 0};

static const scanvideo_view_t vga_view_640x480 = {
    .default_timing = &vga_timing_640x480_60_cea,
    .width = 640,
    .height = 480,
    .x_scale = 1,
    .y_scale = 1,
    .y_offset = 0};

static const scanvideo_view_t vga_view_320x180 = {
    .default_timing = &vga_timing_640x480_60_cea,
    .width = 320,
    .height = 180,
    .x_scale = 2,
    .y_scale = 2,
    .y_offset = 60};

static const scanvideo_view_t vga_view_640x360 = {
    .default_timing = &vga_timing_640x480_60_cea,
    .width = 640,
    .height = 360,
    .x_scale = 1,
    .y_scale = 1,
    .y_offset = 60};

static const scanvideo_view_t vga_view_320x200 = {
    .default_timing = &vga_timing_640x480_60_cea,
    .width = 320,
    .height = 200,
    .x_scale = 2,
    .y_scale = 2,
    .y_offset = 40};

static const scanvideo_view_t vga_view_640x400 = {
    .default_timing = &vga_timing_640x480_60_cea,
    .width = 640,
    .height = 400,
    .x_scale = 1,
    .y_scale = 1,
    .y_offset = 40};

static const scanvideo_view_t vga_view_320x240_sxga = {
    .default_timing = &vga_timing_1280x1024_60_dmt,
    .width = 320,
    .height = 240,
    .x_scale = 2,
    .y_scale = 4,
    .y_offset = 32};

static const scanvideo_view_t vga_view_640x480_sxga = {
    .default_timing = &vga_timing_1280x1024_60_dmt,
    .width = 640,
    .height = 480,
    .x_scale = 1,
    .y_scale = 2,
    .y_offset = 32};

static const scanvideo_view_t vga_view_640x512_sxga = {
    .default_timing = &vga_timing_1280x1024_60_dmt,
    .width = 640,
    .height = 512,
    .x_scale = 1,
    .y_scale = 2,
    .y_offset = 0};

static const scanvideo_view_t vga_view_320x180_sxga = {
    .default_timing = &vga_timing_1280x1024_60_dmt,
    .width = 320,
    .height = 180,
    .x_scale = 2,
    .y_scale = 4,
    .y_offset = 152};

static const scanvideo_view_t vga_view_640x360_sxga = {
    .default_timing = &vga_timing_1280x1024_60_dmt,
    .width = 640,
    .height = 360,
    .x_scale = 1,
    .y_scale = 2,
    .y_offset = 152};

static const scanvideo_view_t vga_view_320x200_sxga = {
    .default_timing = &vga_timing_1280x1024_60_dmt,
    .width = 320,
    .height = 200,
    .x_scale = 2,
    .y_scale = 4,
    .y_offset = 112};

static const scanvideo_view_t vga_view_640x400_sxga = {
    .default_timing = &vga_timing_1280x1024_60_dmt,
    .width = 640,
    .height = 400,
    .x_scale = 1,
    .y_scale = 2,
    .y_offset = 112};

static const scanvideo_view_t vga_view_320x180_hd = {
    .default_timing = &vga_timing_1280x720_60_cea,
    .width = 320,
    .height = 180,
    .x_scale = 2,
    .y_scale = 4,
    .y_offset = 0};

static const scanvideo_view_t vga_view_640x360_hd = {
    .default_timing = &vga_timing_1280x720_60_cea,
    .width = 640,
    .height = 360,
    .x_scale = 1,
    .y_scale = 2,
    .y_offset = 0};

static void vga_scanvideo_switch(void)
{
    if (!vga_view_switching)
        return;

    // Prevent new renders from starting, then wait for any in-flight
    // render to finish, since shared state including spinlocks gets zeroed.
    mutex_enter_blocking(&vga_scanline_mutex);
    while (vga_rendering[0] || vga_rendering[1])
        tight_loop_contents();

    // Set system clock for the new video mode; must run before scanvideo_set_mode()
    uint32_t clk = vga_view_selected->default_timing->clock_freq;
    if (clk == 25200000)
#if PICO_RP2040
        /* Pico 1 port: the M0+ renders noticeably less per clock than the
         * RP2350's M33 and misses scanlines at 201.6 MHz (blue lines, sprite
         * holes), so 640x480 runs at 10x the pixel clock instead of 8x. */
        clk = 25200000 * 10; // 252.0 MHz
#else
        clk = 25200000 * 8; // 201.6 MHz
#endif
    else if (clk == 54000000)
        clk = 54000000 * 4; // 216.0 MHz
    else if (clk == 37125000)
        clk = 37125000 * 4; // 148.5 MHz
    assert(clk >= 120000000 && clk <= 266000000);
    if (clk != clock_get_hz(clk_sys))
    {
        main_pre_reclock();
#if PICO_RP2040
        /* Pico 1 port: the RP2040 is rated for 133 MHz and VGA runs it at up
         * to 252 MHz. 1.20 V is the usual margin to 216 MHz and 1.25 V above
         * it; raise it before the clock goes up and let the regulator settle. */
        vreg_set_voltage(clk > 216000000 ? VREG_VOLTAGE_1_25 : VREG_VOLTAGE_1_20);
        busy_wait_us(1000);
#endif
        set_sys_clock_khz(clk / 1000, true);
        main_post_reclock();
    }

    scanvideo_set_mode(vga_view_selected);

    vga_view_current = vga_view_selected;
    vga_view_switching = false;

    mutex_exit(&vga_scanline_mutex);
}

static unsigned long vga_frames;

static void __not_in_flash_func(vga_scanline_complete)(uint32_t scanline_id)
{
    int16_t scanline = scanvideo_scanline_number(scanline_id);
    int16_t view_height = vga_view_current->height;
    int16_t highest = vga_prog_highest() > 0 && vga_prog_highest() <= view_height
                          ? vga_prog_highest()
                          : view_height;
    if (scanline + 1 < highest && scanline != 0)
        return;
    uint16_t frame = scanvideo_frame_number(scanline_id);
    if (scanline == 0 && scanline + 1 < highest)
        // scanvideo can skip past the previous frame's highest scanline, so
        // that frame's VSYNC is sent when scanline 0 completes if it was not
        // sent already.
        --frame;
    // Both cores complete scanlines, so vga_vsync_frame_fired is exchanged
    // atomically. Each call to ria_vsync sends the RIA a new frame number,
    // and the RIA triggers a VSYNC for every new frame number, so a second
    // call for one frame would trigger an extra VSYNC on the RIA.
    if (__atomic_exchange_n(&vga_vsync_frame_fired, frame, __ATOMIC_ACQ_REL) != frame)
    {
        __atomic_fetch_add(&vga_frames, 1, __ATOMIC_RELAXED);
        ria_vsync();
    }
}

/* Relaxed ordering is enough because the terminal uses this count only to
 * time its cursor and cell blinks, and no other data is published with it. */
unsigned long vga_frame_count(void)
{
    return __atomic_load_n(&vga_frames, __ATOMIC_RELAXED);
}

#if PICO1_DEBUG
/* Pico 1 port debug: render time per scanline, worst case per core. */
extern volatile uint32_t scanvideo_dbg_missed;
static volatile uint32_t vga_dbg_render_max[2], vga_dbg_render_n[2];
/* Pico 1 port debug: the slowest line since the last print, with the time
 * each plane's fill (f) and sprites (s) took on it, in microseconds. Held
 * until printed, so a line from a stretch when core 0 is too busy to print
 * still shows once it frees up. */
static volatile struct
{
    uint32_t total;
    int16_t y;
    uint16_t fill[SCANVIDEO_PLANE_COUNT], sprite[SCANVIDEO_PLANE_COUNT];
} vga_dbg_worst;
#endif

static void vga_render_scanline(void)
{
    if (!mutex_try_enter(&vga_scanline_mutex, 0))
        return;
    scanvideo_scanline_buffer_t *const scanline_buffer =
        scanvideo_begin_scanline_generation();
    if (!scanline_buffer)
    {
        mutex_exit(&vga_scanline_mutex);
        return;
    }
    vga_rendering[get_core_num()] = true;
    mutex_exit(&vga_scanline_mutex);
#if PICO1_DEBUG
    const uint32_t dbg_t0 = timer_hw->timerawl;
#endif

    const uint16_t width = vga_view_current->width;
    const uint32_t buffer_scanline_id = scanline_buffer->scanline_id;
    const int16_t scanline_id = scanvideo_scanline_number(buffer_scanline_id);
    uint32_t *const data[SCANVIDEO_PLANE_COUNT] = {scanline_buffer->data0, scanline_buffer->data1, scanline_buffer->data2};
    bool filled[SCANVIDEO_PLANE_COUNT] = {false, false, false};
    uint32_t *foreground = NULL;
    vga_prog_t prog = *vga_prog_row(scanline_id);
#if PICO1_DEBUG
    uint16_t dbg_fill[SCANVIDEO_PLANE_COUNT] = {0}, dbg_sprite[SCANVIDEO_PLANE_COUNT] = {0};
    uint32_t dbg_t = timer_hw->timerawl;
#define VGA_DBG_LAP(arr, i)                         \
    do                                              \
    {                                               \
        const uint32_t now = timer_hw->timerawl;    \
        (arr)[i] = (uint16_t)(now - dbg_t);         \
        dbg_t = now;                                \
    } while (0)
#else
#define VGA_DBG_LAP(arr, i) ((void)0)
#endif
    for (int i = 0; i < SCANVIDEO_PLANE_COUNT; i++)
    {
        if (prog.fill_fn[i])
        {
            filled[i] = prog.fill_fn[i](i,
                                        scanline_id,
                                        width,
                                        (uint16_t *)(data[i] + 1),
                                        prog.fill_config[i]);
            if (filled[i])
                foreground = data[i];
            VGA_DBG_LAP(dbg_fill, i);
        }
        if (prog.sprite_fn[i])
        {
            if (!foreground)
            {
                foreground = data[i];
                memset(foreground + 1, 0, width * 2);
                filled[i] = true;
            }
            prog.sprite_fn[i](scanline_id,
                              width,
                              (uint16_t *)(foreground + 1),
                              prog.sprite_config[i],
                              prog.sprite_length[i]);
            VGA_DBG_LAP(dbg_sprite, i);
        }
    }
#undef VGA_DBG_LAP
#ifdef VGA_CAP
    if (cap_state == CAP_WANT && scanline_id == cap_line)
    {
        uint8_t want = CAP_WANT;
        if (__atomic_compare_exchange_n(&cap_state, &want, CAP_COPYING, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        {
            uint16_t *const planes[SCANVIDEO_PLANE_COUNT] = {
                (uint16_t *)(data[0] + 1), (uint16_t *)(data[1] + 1), (uint16_t *)(data[2] + 1)};
            cap_store(scanvideo_frame_number(buffer_scanline_id), scanline_id,
                      width, vga_view_current->height, planes, filled);
        }
    }
#endif
    for (int i = 0; i < SCANVIDEO_PLANE_COUNT; i++)
    {
        uint16_t data_used;
        if (filled[i])
        {
            data[i][0] = COMPOSABLE_RAW_RUN | (data[i][1] << 16);
            data[i][1] = (width - 3) | (data[i][1] & 0xFFFF0000);
            data[i][width / 2 + 1] = COMPOSABLE_RAW_1P | (0 << 16);
            data[i][width / 2 + 2] = COMPOSABLE_EOL_SKIP_ALIGN;
            data_used = width / 2 + 3;
        }
        else
        {
            data[i][0] = COMPOSABLE_RAW_1P | (0 << 16);
            data[i][1] = COMPOSABLE_EOL_SKIP_ALIGN;
            data_used = 2;
        }
        switch (i)
        {
        case 0:
            scanline_buffer->data0_used = data_used;
            break;
        case 1:
            scanline_buffer->data1_used = data_used;
            break;
        case 2:
            scanline_buffer->data2_used = data_used;
            break;
        }
    }
#if PICO1_DEBUG
    {
        const uint32_t dt = timer_hw->timerawl - dbg_t0;
        const uint core = get_core_num();
        if (dt > vga_dbg_render_max[core])
            vga_dbg_render_max[core] = dt;
        if (dt > vga_dbg_worst.total)
        {
            vga_dbg_worst.total = dt;
            vga_dbg_worst.y = scanline_id;
            for (int i = 0; i < SCANVIDEO_PLANE_COUNT; i++)
            {
                vga_dbg_worst.fill[i] = dbg_fill[i];
                vga_dbg_worst.sprite[i] = dbg_sprite[i];
            }
        }
        vga_dbg_render_n[core]++;
    }
#endif
    scanvideo_end_scanline_generation(scanline_buffer);
    vga_scanline_complete(buffer_scanline_id);
    vga_rendering[get_core_num()] = false;
}

// [canvas][display]. vga_xreg_canvas accepts exactly the canvases that have
// a row here, and the assert holds the rows to the list in core/vga/vga.h.
__in_flash("vga_canvas_view_table") static const scanvideo_view_t *const vga_canvas_view_table[][3] = {
    [vga_canvas_console] = {[vga_sd] = &vga_view_640x480,
                            [vga_hd] = &vga_view_640x480,
                            [vga_sxga] = &vga_view_640x512_sxga},
    [vga_canvas_320_240] = {[vga_sd] = &vga_view_320x240,
                            [vga_hd] = &vga_view_320x240,
                            [vga_sxga] = &vga_view_320x240_sxga},
    [vga_canvas_320_180] = {[vga_sd] = &vga_view_320x180,
                            [vga_hd] = &vga_view_320x180_hd,
                            [vga_sxga] = &vga_view_320x180_sxga},
    [vga_canvas_640_480] = {[vga_sd] = &vga_view_640x480,
                            [vga_hd] = &vga_view_640x480,
                            [vga_sxga] = &vga_view_640x480_sxga},
    [vga_canvas_640_360] = {[vga_sd] = &vga_view_640x360,
                            [vga_hd] = &vga_view_640x360_hd,
                            [vga_sxga] = &vga_view_640x360_sxga},
    [vga_canvas_320_200] = {[vga_sd] = &vga_view_320x200,
                            [vga_hd] = &vga_view_320x200,
                            [vga_sxga] = &vga_view_320x200_sxga},
    [vga_canvas_640_400] = {[vga_sd] = &vga_view_640x400,
                            [vga_hd] = &vga_view_640x400,
                            [vga_sxga] = &vga_view_640x400_sxga},
};
_Static_assert(sizeof vga_canvas_view_table / sizeof vga_canvas_view_table[0] == VGA_CANVAS_COUNT,
               "a canvas without a view");

static void vga_scanvideo_update(void)
{
    vga_view_selected = vga_canvas_view_table[vga_canvas_selected][vga_display_selected];
    if (vga_view_selected != vga_view_current)
        vga_view_switching = true;
}

static void vga_reset_console_prog(void)
{
    uint16_t xregs_console[] = {0, vga_canvas_console, 0, 0, 0};
    main_prog(xregs_console);
}

void vga_set_display(vga_display_t display)
{
    vga_display_selected = display;
    vga_scanvideo_update();
    if (vga_view_switching && vga_canvas_selected == vga_canvas_console)
        vga_reset_console_prog();
}

// Also accepts NULL for reset to vga_canvas_console.
// When xregs is non-NULL (pix xreg), sends ACK/NAK via backchannel.
void vga_xreg_canvas(uint16_t *xregs)
{
    uint16_t canvas = xregs ? xregs[0] : vga_canvas_console;
    if (canvas >= VGA_CANVAS_COUNT)
    {
        if (xregs)
            ria_nak();
        return;
    }
    // prevent flicker when reset not needed
    if (canvas == vga_canvas_console && vga_canvas_selected == vga_canvas_console)
    {
        if (xregs)
            ria_ack();
        return;
    }
    vga_prog_reset();
    vga_canvas_selected = (vga_canvas_t)canvas;
    vga_scanvideo_update();
    if (canvas == vga_canvas_console)
        vga_reset_console_prog();
    if (xregs)
        ria_ack();
}

int16_t vga_canvas_height(void)
{
    return vga_view_selected->height;
}

int16_t vga_canvas_width(void)
{
    return vga_view_selected->width;
}

static void vga_render_loop(void)
{
    while (true)
        vga_render_scanline();
}

void vga_init(void)
{
    // xram must be 64K-aligned for 6502 address wrap
    assert(!((uintptr_t)xram & 0xFFFF));

    mutex_init(&vga_scanline_mutex);
    vga_set_display(vga_sd);
    vga_scanvideo_switch();
}

void vga_task(void)
{
    vga_scanvideo_switch();
    vga_render_scanline();
#if PICO1_DEBUG
    /* Two short lines on alternate seconds: the console's buffer is 128
     * bytes and com_dbg_out drops anything over half its free room. */
    static absolute_time_t dbg_next;
    static uint32_t dbg_missed_last;
    static bool dbg_worst_turn;
    if (time_reached(dbg_next))
    {
        dbg_next = make_timeout_time_ms(1000);
        char buf[64];
        int n;
        if (dbg_worst_turn)
        {
            n = snprintf(buf, sizeof(buf), "[vgaw y=%d %lu f=%u/%u/%u s=%u/%u/%u]\r\n",
                         vga_dbg_worst.y, (unsigned long)vga_dbg_worst.total,
                         vga_dbg_worst.fill[0], vga_dbg_worst.fill[1], vga_dbg_worst.fill[2],
                         vga_dbg_worst.sprite[0], vga_dbg_worst.sprite[1], vga_dbg_worst.sprite[2]);
            if (com_dbg_out(buf, n))
                vga_dbg_worst.total = 0;
        }
        else
        {
            uint32_t missed = scanvideo_dbg_missed;
            n = snprintf(buf, sizeof(buf), "\r\n[vga miss=%lu +%lu max0=%luus max1=%luus n0=%lu n1=%lu]\r\n",
                         (unsigned long)missed, (unsigned long)(missed - dbg_missed_last),
                         (unsigned long)vga_dbg_render_max[0], (unsigned long)vga_dbg_render_max[1],
                         (unsigned long)vga_dbg_render_n[0], (unsigned long)vga_dbg_render_n[1]);
            dbg_missed_last = missed;
            vga_dbg_render_max[0] = vga_dbg_render_max[1] = 0;
            vga_dbg_render_n[0] = vga_dbg_render_n[1] = 0;
            com_dbg_out(buf, n);
        }
        dbg_worst_turn = !dbg_worst_turn;
    }
#endif
}

bool vga_canvas_is_console(void)
{
    return vga_canvas_selected == vga_canvas_console;
}

#pragma GCC pop_options

void vga_start_render_core(void)
{
    multicore_launch_core1(vga_render_loop);
}
