/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_SYS_CAP_H_
#define _VGA_SYS_CAP_H_

/* Pico 1 port debug: scanline capture over the USB console.
 *
 * The host sends a command by setting the CDC line coding to the bit rate
 * 0x5A000000 | cmd << 16 | arg, out of band of the console data:
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

#if PICO1_DEBUG
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

#define CAP_RATE_TAG 0x5A

// The CDC line coding changed; bit rates tagged CAP_RATE_TAG are commands.
void cap_line_coding(uint32_t bit_rate);

// Core 0. True while a packet is going out, so console output waits.
bool cap_task(void);

#endif /* VGA_CAP */
#endif /* _VGA_SYS_CAP_H_ */
