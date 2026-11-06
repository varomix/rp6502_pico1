/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/vga/prog.h"
#include <stddef.h>

#pragma GCC push_options
#pragma GCC optimize("O3")

#if PICO_RP2040
/* Pico 1 port: compact scanline program.
 *
 * A vga_prog_t row is 44 bytes, 24 of them function pointers, and there are
 * VGA_PROG_MAX rows: 22.5 KB the RP2040 cannot spare. Only a few dozen
 * distinct renderers exist, so rows store a one-byte index into an
 * append-only table of them instead (0 is NULL), and a row is 24 bytes.
 * vga_prog_row decodes into a per-core buffer: core 1 renders, and core 0
 * renders and saves state from the same loop, never at once. The table is
 * written before any row refers to an entry, so core 1 never sees an index
 * whose pointer is not yet there. */
#include <pico.h>
#include <hardware/sync.h>
#define VGA_PROG_FNS_MAX 64

typedef struct
{
    uint8_t fill_fn[SCANVIDEO_PLANE_COUNT];
    uint8_t sprite_fn[SCANVIDEO_PLANE_COUNT];
    uint16_t fill_config[SCANVIDEO_PLANE_COUNT];
    uint16_t sprite_config[SCANVIDEO_PLANE_COUNT];
    uint16_t sprite_length[SCANVIDEO_PLANE_COUNT];
} vga_prog_packed_t;

static vga_prog_packed_t vga_prog[VGA_PROG_MAX];
static vga_fill_fn_t vga_prog_fill_fns[VGA_PROG_FNS_MAX];
static vga_sprite_fn_t vga_prog_sprite_fns[VGA_PROG_FNS_MAX];
static uint8_t vga_prog_fill_fns_used = 1;
static uint8_t vga_prog_sprite_fns_used = 1;
static vga_prog_t vga_prog_decoded[2];

static uint8_t vga_prog_intern(void **table, uint8_t *used, void *fn)
{
    if (!fn)
        return 0;
    for (uint8_t i = 1; i < *used; i++)
        if (table[i] == fn)
            return i;
    if (*used >= VGA_PROG_FNS_MAX)
        return 0; // table full: renders nothing rather than the wrong thing
    table[*used] = fn;
    __dmb(); // pointer visible before any row names it
    return (*used)++;
}

static uint8_t vga_prog_fill_id(vga_fill_fn_t fn)
{
    return vga_prog_intern((void **)vga_prog_fill_fns, &vga_prog_fill_fns_used, (void *)fn);
}

static uint8_t vga_prog_sprite_id(vga_sprite_fn_t fn)
{
    return vga_prog_intern((void **)vga_prog_sprite_fns, &vga_prog_sprite_fns_used, (void *)fn);
}

static int16_t vga_highest_scanline;

const vga_prog_t *vga_prog_row(int16_t scanline)
{
    const vga_prog_packed_t *r = &vga_prog[scanline];
    vga_prog_t *d = &vga_prog_decoded[get_core_num()];
    for (int i = 0; i < SCANVIDEO_PLANE_COUNT; i++)
    {
        d->fill_fn[i] = vga_prog_fill_fns[r->fill_fn[i]];
        d->fill_config[i] = r->fill_config[i];
        d->sprite_fn[i] = vga_prog_sprite_fns[r->sprite_fn[i]];
        d->sprite_config[i] = r->sprite_config[i];
        d->sprite_length[i] = r->sprite_length[i];
    }
    return d;
}

void vga_prog_reset(void)
{
    for (uint16_t i = 0; i < VGA_PROG_MAX; i++)
        vga_prog[i] = (vga_prog_packed_t){0};
    vga_highest_scanline = 0;
}

#define VGA_PROG_SET_FILL(row, plane, fn) ((row).fill_fn[plane] = vga_prog_fill_id(fn))
#define VGA_PROG_SET_SPRITE(row, plane, fn) ((row).sprite_fn[plane] = vga_prog_sprite_id(fn))
#define VGA_PROG_FILL_IS(row, plane, fn) ((row).fill_fn[plane] && \
    vga_prog_fill_fns[(row).fill_fn[plane]] == (fn))

#else /* !PICO_RP2040 */

static vga_prog_t vga_prog[VGA_PROG_MAX];

static int16_t vga_highest_scanline;

const vga_prog_t *vga_prog_row(int16_t scanline)
{
    return &vga_prog[scanline];
}

void vga_prog_reset(void)
{
    for (uint16_t i = 0; i < VGA_PROG_MAX; i++)
        vga_prog[i] = (vga_prog_t){0};
    vga_highest_scanline = 0;
}

#define VGA_PROG_SET_FILL(row, plane, fn) ((row).fill_fn[plane] = (fn))
#define VGA_PROG_SET_SPRITE(row, plane, fn) ((row).sprite_fn[plane] = (fn))
#define VGA_PROG_FILL_IS(row, plane, fn) ((row).fill_fn[plane] == (fn))

#endif /* PICO_RP2040 */

int16_t vga_prog_highest(void)
{
    return vga_highest_scanline;
}

void vga_prog_load_row(int16_t scanline, const vga_prog_t *row)
{
    if (scanline >= 0 && scanline < VGA_PROG_MAX)
    {
#if PICO_RP2040
        vga_prog_packed_t *r = &vga_prog[scanline];
        for (int i = 0; i < SCANVIDEO_PLANE_COUNT; i++)
        {
            r->fill_config[i] = row->fill_config[i];
            r->sprite_config[i] = row->sprite_config[i];
            r->sprite_length[i] = row->sprite_length[i];
            VGA_PROG_SET_FILL(*r, i, row->fill_fn[i]);
            VGA_PROG_SET_SPRITE(*r, i, row->sprite_fn[i]);
        }
#else
        vga_prog[scanline] = *row;
#endif
    }
}

void vga_prog_set_highest(int16_t scanline)
{
    vga_highest_scanline = scanline;
}

bool vga_prog_valid(int16_t plane, int16_t scanline_begin, int16_t *scanline_end)
{
    if (!*scanline_end)
        *scanline_end = vga_canvas_height();
    if (plane < 0 || plane >= SCANVIDEO_PLANE_COUNT ||
        scanline_begin < 0 || *scanline_end > vga_canvas_height() ||
        *scanline_end - scanline_begin < 1)
        return false;
    if (*scanline_end > vga_highest_scanline)
        vga_highest_scanline = *scanline_end;
    return true;
}

bool vga_prog_fill(int16_t plane, int16_t scanline_begin, int16_t scanline_end,
                   uint16_t config_ptr, vga_fill_fn_t fill_fn)
{
    if (vga_canvas_is_console())
        return false;
    if (!vga_prog_valid(plane, scanline_begin, &scanline_end))
        return false;
    for (int16_t i = scanline_begin; i < scanline_end; i++)
    {
        vga_prog[i].fill_config[plane] = config_ptr;
        VGA_PROG_SET_FILL(vga_prog[i], plane, fill_fn);
    }
    return true;
}

bool vga_prog_exclusive(int16_t plane, int16_t scanline_begin, int16_t scanline_end,
                        uint16_t config_ptr, vga_fill_fn_t fill_fn)
{
    if (!vga_prog_valid(plane, scanline_begin, &scanline_end))
        return false;
    for (uint16_t i = 0; i < VGA_PROG_MAX; i++)
        for (uint16_t j = 0; j < SCANVIDEO_PLANE_COUNT; j++)
            if (VGA_PROG_FILL_IS(vga_prog[i], j, fill_fn))
                VGA_PROG_SET_FILL(vga_prog[i], j, NULL);
    for (int16_t i = scanline_begin; i < scanline_end; i++)
    {
        vga_prog[i].fill_config[plane] = config_ptr;
        VGA_PROG_SET_FILL(vga_prog[i], plane, fill_fn);
    }
    return true;
}

bool vga_prog_sprite(int16_t plane, int16_t scanline_begin, int16_t scanline_end,
                     uint16_t config_ptr, uint16_t length, vga_sprite_fn_t sprite_fn)
{
    if (vga_canvas_is_console())
        return false;
    if (!vga_prog_valid(plane, scanline_begin, &scanline_end))
        return false;
    for (int16_t i = scanline_begin; i < scanline_end; i++)
    {
        vga_prog[i].sprite_config[plane] = config_ptr;
        vga_prog[i].sprite_length[plane] = length;
        VGA_PROG_SET_SPRITE(vga_prog[i], plane, sprite_fn);
    }
    return true;
}

#pragma GCC pop_options
