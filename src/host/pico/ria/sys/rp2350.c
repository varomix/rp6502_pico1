/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ria/sys/rp2350.h"
#include <hardware/clocks.h>
#include <hardware/vreg.h>

#if PICO_RP2040
/* Pico 1 port: the RP2040 is rated for 133 MHz. 256 MHz commonly holds at
 * 1.20 V; raise to 1.25 V or 1.30 V if a particular chip is unstable. */
#define SYS_RP2350_VREG VREG_VOLTAGE_1_20
#else
#define SYS_RP2350_VREG VREG_VOLTAGE_1_15
#endif

void rp2350_init(void)
{
    vreg_set_voltage(SYS_RP2350_VREG);
    set_sys_clock_khz(SYS_RP2350_KHZ, true);
}
