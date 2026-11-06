/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_PICO_XIP_UPDATE_H_
#define _HOST_PICO_XIP_UPDATE_H_

/* Firmware update for a build that executes from flash (XIP), as the RP2040
 * builds do. The RP2350 builds run from RAM (copy_to_ram) and program the new
 * image over the old one in place; an XIP build would erase the code doing
 * the programming. So the new image is staged in flash the running image
 * does not use, sector by sector through a map, and xip_update_commit copies
 * it into place from RAM and resets.
 *
 * Staging leaves the running image intact, so a failed staging needs no
 * recovery. Losing power during the commit, a few seconds, needs BOOTSEL. */

#include <pico.h>
#include <stdbool.h>
#include <stdint.h>

#if !PICO_COPY_TO_RAM && !PICO_NO_FLASH
#define XIP_UPDATE 1
#endif

#ifdef XIP_UPDATE

// Destination sectors an update can cover: 1 MB of image.
#define XIP_UPDATE_MAX_SECTORS 256

// Start a session. map[s] is the flash sector holding the staged copy of
// destination sector s, or map is NULL to stage sector s at base_sector + s.
// map must stay valid until commit.
void xip_update_begin(const uint16_t *map, uint16_t base_sector);

// Stage one FLASH_PAGE_SIZE page destined for flash offset offs. Erases a
// staging sector the first time the session touches it, then programs and
// verifies. False if offs is outside the map or the page did not verify.
bool xip_update_page(uint32_t offs, const uint8_t *page);

// Mark destination sectors [0, count) as part of the image, erasing the
// staging sectors of any the session did not touch, so they commit as blank.
void xip_update_fill(uint32_t count);

// Stop core 1, disable interrupts, copy every touched sector into place
// using buf (FLASH_SECTOR_SIZE bytes of RAM whose contents may be lost), and
// reset the chip.
void xip_update_commit(void *buf) __attribute__((noreturn));

#endif /* XIP_UPDATE */
#endif /* _HOST_PICO_XIP_UPDATE_H_ */
