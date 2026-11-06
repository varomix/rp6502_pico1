/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_SYS_FLASH_H_
#define _VGA_SYS_FLASH_H_

#include "core/sys/driver.h"

#include <stdbool.h>
#include <stdint.h>

bool flash_program_request(uint16_t page);
void flash_task(void);

// The RIA's reboot request ends a FLASH update, so an update staged by this
// build (see xip_update.h) is committed here. Does not return.
void flash_reboot(void) __attribute__((noreturn));

#define FLASH_DRIVER DRIVER(nul_init, flash_task, nul_task, nul_run, nul_stop, nul_break, nul_config, nul_config, nul_sst)

#endif /* _VGA_SYS_FLASH_H_ */
