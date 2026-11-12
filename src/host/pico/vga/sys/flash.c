/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/xram.h"
#include "vga/sys/flash.h"
#include "vga/sys/ria.h"
#include "xip_update.h"
#include "vga/sys/com.h"
#include <stdio.h>
#include <hardware/watchdog.h>
#include <hardware/flash.h>
#include <pico.h>
#include <pico/stdlib.h>
#include <string.h>

static volatile bool flash_pending;
static volatile uint16_t flash_page;

#ifdef XIP_UPDATE
// This build executes from flash, so an update is staged in the upper half
// of flash, which also limits an image to the lower half.
#define FLASH_STAGE_OFFS (PICO_FLASH_SIZE_BYTES / 2)
static bool flash_staged;
#define FLASH_PAGES (FLASH_STAGE_OFFS / FLASH_PAGE_SIZE)
#else
#define FLASH_PAGES (PICO_FLASH_SIZE_BYTES / FLASH_PAGE_SIZE)
#endif

bool flash_program_request(uint16_t page)
{
    if ((uint32_t)page >= FLASH_PAGES)
        return false;
    flash_page = page;
    flash_pending = true;
    return true;
}

void flash_task(void)
{
    if (!flash_pending)
        return;
    flash_pending = false;

    const uint32_t offs = (uint32_t)flash_page * FLASH_PAGE_SIZE;
    const uint8_t *src = (const uint8_t *)xram;

#ifdef XIP_UPDATE
    // The RIA sends an image from its first page, which starts a session.
    if (!flash_page || !flash_staged)
        xip_update_begin(NULL, FLASH_STAGE_OFFS / FLASH_SECTOR_SIZE);
    flash_staged = true;
#if PICO1_DEBUG // Pico 1 port debug: result and time on the USB console
    uint32_t t0 = time_us_32();
    bool ok = xip_update_page(offs, src);
    char dbg[64];
    int n = snprintf(dbg, sizeof dbg, "\r\n[vga flash page %u %s %luus]\r\n",
                     flash_page, ok ? "ok" : "FAIL", (unsigned long)(time_us_32() - t0));
    com_dbg_out(dbg, n);
#else
    bool ok = xip_update_page(offs, src);
#endif
    if (ok)
        ria_ack();
    else
        ria_nak();
    return;
#endif

    const uint8_t *dest = (const uint8_t *)(XIP_NOCACHE_NOALLOC_BASE + offs);
    for (uint32_t i = 0; i < FLASH_PAGE_SIZE; i++)
        if (dest[i] != 0xFF)
        {
            flash_range_erase(offs & ~(FLASH_SECTOR_SIZE - 1), FLASH_SECTOR_SIZE);
            break;
        }

    flash_range_program(offs, src, FLASH_PAGE_SIZE);

    if (memcmp(dest, src, FLASH_PAGE_SIZE) == 0)
        ria_ack();
    else
        ria_nak();
}

void flash_reboot(void)
{
#ifdef XIP_UPDATE
    if (flash_staged)
        xip_update_commit((void *)xram);
#endif
    watchdog_reboot(0, 0, 0);
    for (;;)
        tight_loop_contents();
}
