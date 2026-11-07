/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "vga/sys/cap.h"

#ifdef VGA_CAP

#include "vga/sys/com.h"
#include "core/vga/pixel_format.h"
#include "core/sys/xram.h"
#include "vga/sys/flash.h"
#include <tusb.h>
#include <string.h>

/* Packet, little endian:
 *   "VCAP" u16 seq u16 frame u16 line u16 width u16 height u8 mask u8 0
 *   then width u16 pixels for each plane set in mask, plane 0 first. */
#define CAP_HEADER_SIZE 18

volatile uint8_t cap_state = CAP_OFF;
volatile int16_t cap_line;

static uint8_t cap_mode;
static uint16_t cap_arg;
static uint16_t cap_seq;
static uint16_t cap_frame, cap_width, cap_height;
static uint8_t cap_mask;
static uint16_t cap_px[CAP_MAX_PIXELS]; // filled planes, packed in order
static uint8_t cap_header[CAP_HEADER_SIZE];
static uint32_t cap_sent, cap_total;
static int16_t cap_mem_next = -1;  // 'M': next 1 KB XRAM block to send
static int16_t cap_mem_last = -1;  // last block of the 'M' request
static int16_t cap_mem_block = -1; // block going out now, -1 for a line

void __not_in_flash_func(cap_store)(uint16_t frame, int16_t line, uint16_t width, uint16_t height,
                                    uint16_t *const planes[], const bool filled[])
{
    if (width > CAP_MAX_WIDTH)
        width = CAP_MAX_WIDTH;
    uint8_t mask = 0;
    uint32_t used = 0;
    for (int i = 0; i < SCANVIDEO_PLANE_COUNT; i++)
        if (filled[i] && used + width <= CAP_MAX_PIXELS)
        {
            memcpy(cap_px + used, planes[i], width * sizeof(uint16_t));
            used += width;
            mask |= 1u << i;
        }
    cap_frame = frame;
    cap_line = line;
    cap_width = width;
    cap_height = height;
    cap_mask = mask;
    __atomic_store_n(&cap_state, CAP_READY, __ATOMIC_RELEASE);
}

static void cap_command(uint8_t cmd, uint16_t arg)
{
    switch (cmd)
    {
    case 'S':
    case 'L':
        cap_mode = cmd;
        cap_arg = arg;
        cap_line = cmd == 'L' ? (int16_t)arg : 0;
        cap_sent = cap_total = 0;
        __atomic_store_n(&cap_state, CAP_WANT, __ATOMIC_RELEASE);
        break;
    case 'F': // stage XRAM[0..255] as flash page arg, as the RIA's FLASH does
        flash_program_request(arg);
        break;
    case 'M': // XRAM blocks: arg low byte first block, high byte count
        cap_mem_next = arg & 0x3F;
        cap_mem_last = (int16_t)(cap_mem_next + ((arg >> 8) ? (arg >> 8) : 1) - 1);
        if (cap_mem_last > 63)
            cap_mem_last = 63;
        break;
    default:
        cap_mem_next = -1;
        __atomic_store_n(&cap_state, CAP_OFF, __ATOMIC_RELEASE);
        cap_sent = cap_total = 0;
        break;
    }
}

void cap_line_coding(uint32_t bit_rate)
{
    if ((bit_rate >> 24) == CAP_RATE_TAG)
        cap_command((uint8_t)(bit_rate >> 16), (uint16_t)bit_rate);
}

static const uint8_t *cap_byte_ptr(uint32_t pos, uint32_t *avail)
{
    if (pos < CAP_HEADER_SIZE)
    {
        *avail = CAP_HEADER_SIZE - pos;
        return cap_header + pos;
    }
    pos -= CAP_HEADER_SIZE;
    *avail = cap_total - CAP_HEADER_SIZE - pos;
    if (cap_mem_block >= 0)
        return (const uint8_t *)&xram[cap_mem_block * 1024u] + pos;
    return (const uint8_t *)cap_px + pos;
}

bool cap_task(void)
{
    if (cap_total == 0 && cap_mem_next >= 0)
    {
        cap_mem_block = cap_mem_next;
        cap_mem_next = cap_mem_next < cap_mem_last ? cap_mem_next + 1 : -1;
        const uint16_t h[] = {cap_seq++, (uint16_t)cap_mem_block, 1024, 0, 0};
        memcpy(cap_header, "VMEM", 4);
        memcpy(cap_header + 4, h, sizeof(h));
        memset(cap_header + 14, 0, 4);
        cap_sent = 0;
        cap_total = CAP_HEADER_SIZE + 1024;
    }
    if (cap_total == 0)
    {
        cap_mem_block = -1;
        if (__atomic_load_n(&cap_state, __ATOMIC_ACQUIRE) != CAP_READY)
            return false;
        if (!tud_cdc_connected())
        {
            cap_command('0', 0);
            return false;
        }
        int planes = 0;
        for (int i = 0; i < SCANVIDEO_PLANE_COUNT; i++)
            planes += (cap_mask >> i) & 1;
        const uint16_t h[] = {cap_seq++, cap_frame, (uint16_t)cap_line, cap_width, cap_height};
        memcpy(cap_header, "VCAP", 4);
        memcpy(cap_header + 4, h, sizeof(h));
        cap_header[14] = cap_mask;
        cap_header[15] = cap_mode;
        cap_header[16] = cap_header[17] = 0;
        cap_sent = 0;
        cap_total = CAP_HEADER_SIZE + (uint32_t)planes * cap_width * sizeof(uint16_t);
    }
    while (cap_sent < cap_total && tud_cdc_write_available())
    {
        uint32_t avail;
        const uint8_t *p = cap_byte_ptr(cap_sent, &avail);
        uint32_t room = tud_cdc_write_available();
        uint32_t n = avail < room ? avail : room;
        n = tud_cdc_write(p, n);
        if (!n)
            break;
        cap_sent += n;
    }
    tud_cdc_write_flush();
    if (cap_sent < cap_total)
        return true;
    cap_total = 0;
    if (cap_mem_block >= 0)
    {
        cap_mem_block = -1;
        return false;
    }
    if (__atomic_load_n(&cap_state, __ATOMIC_ACQUIRE) != CAP_READY)
        return false; // stopped meanwhile
    if (cap_mode == 'S' && cap_height)
        cap_line = (int16_t)((cap_line + cap_arg) % cap_height);
    __atomic_store_n(&cap_state, CAP_WANT, __ATOMIC_RELEASE);
    return false;
}

#endif /* VGA_CAP */
