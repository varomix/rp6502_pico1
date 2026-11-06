/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_SYS_CAP_H_
#define _VGA_SYS_CAP_H_

/* Pico 1 port debug: scanline capture over the USB console.
 *
 * The host sends 0x1D 0x1D 'C' <cmd> <arg lo> <arg hi> on the CDC:
 *   '0'       stop
 *   'S' n     sweep: capture a line, then the line n further on (mod height)
 *   'L' y     capture canvas line y in every frame it can
 *   'M' n     send XRAM: 1 KB block (n & 0x3F), count n >> 8 (0 means 1),
 *             each as a "VMEM" packet
 * Each captured line is sent as a "VCAP" packet (see cap.c) holding the raw
 * pixels of every filled plane, as rendered, before scanvideo shows them.
 * tools/pico1/vgacap.py drives it and writes PNGs. */

#include <stdbool.h>
#include <stdint.h>
#include <pico.h>

#if PICO_RP2040
#define VGA_CAP 1
#endif

#ifdef VGA_CAP

#define CAP_MAX_WIDTH 640
#define CAP_MAX_PIXELS 1024 // 2 KB: three planes at 320 or one at 640

enum
{
    CAP_OFF,
    CAP_WANT,    // render cores: copy cap_line when it is rendered
    CAP_COPYING, // a render core owns the buffer
    CAP_READY,   // core 0: send it
};

extern volatile uint8_t cap_state;
extern volatile int16_t cap_line;

// Called by a render core that has claimed CAP_WANT -> CAP_COPYING.
void cap_store(uint16_t frame, int16_t line, uint16_t width, uint16_t height,
               uint16_t *const planes[], const bool filled[]);

// Host command byte filter. True if the byte was taken by the filter.
bool cap_filter_in(uint8_t ch);

// Core 0. True while a packet is going out, so console output waits.
bool cap_task(void);

#endif /* VGA_CAP */
#endif /* _VGA_SYS_CAP_H_ */
