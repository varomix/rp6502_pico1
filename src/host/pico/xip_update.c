/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "xip_update.h"

#ifdef XIP_UPDATE

#include <hardware/flash.h>
#include <hardware/structs/psm.h>
#include <hardware/structs/ssi.h>
#include <hardware/structs/watchdog.h>
#include <hardware/sync.h>
#include <pico/bootrom.h>
#include <pico/multicore.h>
#include <string.h>

static const uint16_t *xip_update_map;
static uint16_t xip_update_base;
static uint8_t xip_update_touched[XIP_UPDATE_MAX_SECTORS / 8];

static inline uint32_t xip_update_stage_sector(uint32_t s)
{
    return xip_update_map ? xip_update_map[s] : (uint32_t)xip_update_base + s;
}

static inline bool xip_update_is_touched(uint32_t s)
{
    return xip_update_touched[s / 8] & (1u << (s % 8));
}

// Interrupt handlers may live in flash, so none may run while flash is busy.
static void xip_update_erase(uint32_t offs)
{
    uint32_t save = save_and_disable_interrupts();
    flash_range_erase(offs, FLASH_SECTOR_SIZE);
    restore_interrupts(save);
}

void xip_update_begin(const uint16_t *map, uint16_t base_sector)
{
    xip_update_map = map;
    xip_update_base = base_sector;
    memset(xip_update_touched, 0, sizeof xip_update_touched);
}

bool xip_update_page(uint32_t offs, const uint8_t *page)
{
    const uint32_t s = offs / FLASH_SECTOR_SIZE;
    if (s >= XIP_UPDATE_MAX_SECTORS || offs % FLASH_PAGE_SIZE)
        return false;
    const uint32_t stage = xip_update_stage_sector(s) * FLASH_SECTOR_SIZE + offs % FLASH_SECTOR_SIZE;
    if (!xip_update_is_touched(s))
    {
        xip_update_erase(stage & ~(FLASH_SECTOR_SIZE - 1));
        xip_update_touched[s / 8] |= 1u << (s % 8);
    }
    uint32_t save = save_and_disable_interrupts();
    flash_range_program(stage, page, FLASH_PAGE_SIZE);
    restore_interrupts(save);
    return !memcmp((const void *)(XIP_NOCACHE_NOALLOC_BASE + stage), page, FLASH_PAGE_SIZE);
}

void xip_update_fill(uint32_t count)
{
    for (uint32_t s = 0; s < count && s < XIP_UPDATE_MAX_SECTORS; s++)
        if (!xip_update_is_touched(s))
        {
            xip_update_erase(xip_update_stage_sector(s) * FLASH_SECTOR_SIZE);
            xip_update_touched[s / 8] |= 1u << (s % 8);
        }
}

// Runs with the old image being erased under it, so RAM and bootrom only.
// Not even the SDK flash functions: they are RAM functions, but they call
// hard_assert, which is in flash (and was in the 32nd sector on the first
// hardware test, where the copy died). The bootrom functions need no boot2:
// each staged sector is read through the bootrom's own slow XIP mode.
// Serial 03h reads through the bootrom's XIP mode, which the flash rates to
// 50 MHz, so the SPI clock is set to clk_sys / 8: 32 MHz at 256 MHz.
static void __not_in_flash_func(xip_update_read_mode)(
    rom_connect_internal_flash_fn connect, rom_flash_exit_xip_fn exit_xip,
    rom_flash_flush_cache_fn flush_cache, rom_flash_enter_cmd_xip_fn enter_cmd_xip)
{
    connect();
    exit_xip();
    flush_cache();
    enter_cmd_xip();
    ssi_hw->ssienr = 0;
    ssi_hw->baudr = 8;
    ssi_hw->ssienr = 1;
}

static void __no_inline_not_in_flash_func(xip_update_copy)(uint32_t *buf)
{
#if !PICO_RP2040
#error "xip_update_copy uses the RP2040 bootrom flash API"
#endif
    rom_connect_internal_flash_fn connect =
        (rom_connect_internal_flash_fn)rom_func_lookup_inline(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    rom_flash_exit_xip_fn exit_xip =
        (rom_flash_exit_xip_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_EXIT_XIP);
    rom_flash_range_erase_fn erase =
        (rom_flash_range_erase_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_RANGE_ERASE);
    rom_flash_range_program_fn program =
        (rom_flash_range_program_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_RANGE_PROGRAM);
    rom_flash_flush_cache_fn flush_cache =
        (rom_flash_flush_cache_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_FLUSH_CACHE);
    rom_flash_enter_cmd_xip_fn enter_cmd_xip =
        (rom_flash_enter_cmd_xip_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_ENTER_CMD_XIP);

    for (uint32_t s = 0; s < XIP_UPDATE_MAX_SECTORS; s++)
    {
        if (!(xip_update_touched[s / 8] & (1u << (s % 8))))
            continue;
        const uint32_t stage = xip_update_map ? xip_update_map[s] : (uint32_t)xip_update_base + s;
        const volatile uint32_t *src =
            (const volatile uint32_t *)(XIP_NOCACHE_NOALLOC_BASE + stage * FLASH_SECTOR_SIZE);
        const volatile uint32_t *dst =
            (const volatile uint32_t *)(XIP_NOCACHE_NOALLOC_BASE + s * FLASH_SECTOR_SIZE);
        xip_update_read_mode(connect, exit_xip, flush_cache, enter_cmd_xip);
        for (uint32_t i = 0; i < FLASH_SECTOR_SIZE / 4; i++)
            buf[i] = src[i];
        for (int attempt = 0; attempt < 3; attempt++)
        {
            connect();
            exit_xip();
            erase(s * FLASH_SECTOR_SIZE, FLASH_SECTOR_SIZE, FLASH_BLOCK_SIZE, 0xd8);
            program(s * FLASH_SECTOR_SIZE, (const uint8_t *)buf, FLASH_SECTOR_SIZE);
            xip_update_read_mode(connect, exit_xip, flush_cache, enter_cmd_xip);
            uint32_t i = 0;
            while (i < FLASH_SECTOR_SIZE / 4 && dst[i] == buf[i])
                i++;
            if (i == FLASH_SECTOR_SIZE / 4)
                break;
        }
    }
    // watchdog_reboot(0, 0, 0), which lives in flash
    hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);
    watchdog_hw->scratch[4] = 0;
    hw_set_bits(&psm_hw->wdsel, PSM_WDSEL_BITS & ~(PSM_WDSEL_ROSC_BITS | PSM_WDSEL_XOSC_BITS));
    hw_set_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_TRIGGER_BITS);
    for (;;)
        __asm volatile("nop");
}

void xip_update_commit(void *buf)
{
    multicore_reset_core1();
    (void)save_and_disable_interrupts();
    xip_update_copy((uint32_t *)buf);
    __builtin_unreachable();
}

#endif /* XIP_UPDATE */
