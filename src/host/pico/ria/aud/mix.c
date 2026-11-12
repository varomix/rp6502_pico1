/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/aud/mix.h"
#include "core/aud/opl.h"
#include "core/aud/psg.h"
#include "core/aud/bel.h"
#include "core/aud/sine.h"
#include "core/sys/ria.h"
#include "ria/sys/ria.h"
#include "ria/sys/rp2350.h"
#include <pico/stdlib.h>
#include <hardware/pwm.h>
#include <hardware/clocks.h>
#include <hardware/irq.h>
#include <stdio.h>
#include <string.h>

/* Pico 1 port: the RP2040 has a single PWM wrap IRQ. */
#if PICO_RP2040 && !defined(PWM_IRQ_WRAP_0)
#define PWM_IRQ_WRAP_0 PWM_IRQ_WRAP
#endif

#define AUD_L_PIN 28
#define AUD_R_PIN 27
#define AUD_PWM_IRQ_PIN 14 /* No IO */
#define AUD_IRQ_SLICE (pwm_gpio_to_slice_num(AUD_PWM_IRQ_PIN))
#define AUD_L_CHAN (pwm_gpio_to_channel(AUD_L_PIN))
#define AUD_L_SLICE (pwm_gpio_to_slice_num(AUD_L_PIN))
#define AUD_R_CHAN (pwm_gpio_to_channel(AUD_R_PIN))
#define AUD_R_SLICE (pwm_gpio_to_slice_num(AUD_R_PIN))

static aud_dev_t aud_dev;

#ifndef AUD_DRAIN_MAX
#define AUD_DRAIN_MAX RIA_AUD_RING_SIZE
#endif

/* Everything the RW engine left since the last interrupt, which is a dozen
 * writes from a 6502 in a tight store loop and nothing at all the rest of the
 * time. It runs before the sample so that a write takes effect on the sample
 * it arrived for, as it does in the fabric.
 *
 * AUD_DRAIN_MAX bounds the writes taken per sample. A YM3812 accepts about
 * one data write per sample period, so a few is generous, and a 6502 that
 * writes faster only fills the ring (core 1 drops the excess) instead of
 * pushing this interrupt past its period and starving core 0 for good. */
static void aud_drain(void)
{
    uint8_t loc, val;
    for (unsigned n = 0; n < AUD_DRAIN_MAX && ria_aud_next(&loc, &val); n++)
        switch (aud_dev)
        {
        case aud_dev_psg: psg_xram_write(loc, val); break;
        case aud_dev_opl: opl_xram_write(loc, val); break;
        case aud_dev_none: break;
        }
}

static uint16_t aud_level_l = AUD_PWM_CENTER;
static uint16_t aud_level_r = AUD_PWM_CENTER;

#if PICO_RP2040
/* Pico 1 port debug: the IRQ slice counts system clocks from its wrap, so its
 * counter at the end of aud_irq is the cycles the sample took, interrupt
 * latency included. A wrap flag already set again on exit is a lost sample.
 * STATUS prints and clears these. */
static volatile uint32_t aud_dbg_n, aud_dbg_sum, aud_dbg_max, aud_dbg_over, aud_dbg_skip;
extern uint32_t emu8950_prof[3];

/* Late-entry log: aud_irq_entry saves the PC the interrupt returns to, which
 * is the instruction after whatever held it off (a cpsie) or, when it nested,
 * inside the interrupt that did. Samples entered over half a period late, or
 * that overran, keep that PC and the entry count. STATUS prints and clears. */
#define AUD_DBG_LATE 6
__attribute__((used)) volatile uint32_t aud_dbg_entry_pc;
static volatile uint32_t aud_dbg_late_pc[AUD_DBG_LATE];
static volatile uint16_t aud_dbg_late_cnt[AUD_DBG_LATE];
static volatile uint32_t aud_dbg_late_n;
#endif

/* Set when a sample overran its period. The next interrupt then only writes
 * the held level, so an overloaded engine costs a held sample, never all of
 * core 0. */
static bool aud_overran;

/* The levels computed by the previous interrupt are written first, so the
 * write lands at a fixed offset from the interrupt however long the sample
 * generators take after it. The narrowing from sixteen bits to ten rounds
 * rather than floors, because a floor biases every sample down by half a
 * step on average, which is a DC offset and not noise. */
__attribute__((used)) void __isr __time_critical_func(aud_irq)(void)
{
#if PICO_RP2040
    const uint32_t entry = pwm_get_counter(AUD_IRQ_SLICE);
#endif
    pwm_set_chan_level(AUD_L_SLICE, AUD_L_CHAN, aud_level_l);
    pwm_set_chan_level(AUD_R_SLICE, AUD_R_CHAN, aud_level_r);
    pwm_clear_irq(AUD_IRQ_SLICE);

    if (aud_overran)
    {
        aud_overran = false;
#if PICO_RP2040
        aud_dbg_skip++;
#endif
        return;
    }

    aud_drain();
    int16_t l = 0, r = 0;
    switch (aud_dev)
    {
    case aud_dev_psg: psg_sample(&l, &r); break;
    case aud_dev_opl: opl_stereo(&l, &r); break;
    case aud_dev_none: break;
    }
    const int32_t bel = bel_sample();
    int32_t sl = l + bel;
    int32_t sr = r + bel;
    if (sl < AUD_SAMPLE_MIN)
        sl = AUD_SAMPLE_MIN;
    if (sl > AUD_SAMPLE_MAX)
        sl = AUD_SAMPLE_MAX;
    if (sr < AUD_SAMPLE_MIN)
        sr = AUD_SAMPLE_MIN;
    if (sr > AUD_SAMPLE_MAX)
        sr = AUD_SAMPLE_MAX;

    const int shift = 16 - AUD_PWM_BITS;
    int nl = (sl + (1 << (shift - 1))) >> shift;
    int nr = (sr + (1 << (shift - 1))) >> shift;
    if (nl > (int)AUD_PWM_CENTER - 1)
        nl = AUD_PWM_CENTER - 1;
    if (nr > (int)AUD_PWM_CENTER - 1)
        nr = AUD_PWM_CENTER - 1;
    aud_level_l = (uint16_t)(nl + AUD_PWM_CENTER);
    aud_level_r = (uint16_t)(nr + AUD_PWM_CENTER);
    aud_overran = pwm_hw->intr & (1u << AUD_IRQ_SLICE);
#if PICO_RP2040
    const uint32_t c = pwm_get_counter(AUD_IRQ_SLICE);
    if (pwm_hw->intr & (1u << AUD_IRQ_SLICE))
        aud_dbg_over++;
    if (entry > (pwm_hw->slice[AUD_IRQ_SLICE].top + 1) / 2 || (pwm_hw->intr & (1u << AUD_IRQ_SLICE)))
    {
        const uint32_t i = aud_dbg_late_n++ % AUD_DBG_LATE;
        aud_dbg_late_pc[i] = aud_dbg_entry_pc;
        aud_dbg_late_cnt[i] = (uint16_t)entry;
    }
    aud_dbg_n++;
    aud_dbg_sum += c;
    if (c > aud_dbg_max)
        aud_dbg_max = c;
#endif
}

#if PICO_RP2040
/* Thread mode runs on MSP, so the stacked PC is at MSP+24 on entry. */
__attribute__((naked)) void __time_critical_func(aud_irq_entry)(void)
{
    __asm volatile(
        "mrs r0, msp\n"
        "ldr r1, [r0, #24]\n"
        "ldr r2, =aud_dbg_entry_pc\n"
        "str r1, [r2]\n"
        "ldr r0, =aud_irq\n"
        "bx r0\n"
        ".ltorg\n");
}

int aud_status_response(char *buf, size_t buf_size, int state, unsigned width)
{
    (void)width;
    if (state == 1)
    {
        irq_set_enabled(PWM_IRQ_WRAP_0, false);
        const uint32_t ln = aud_dbg_late_n;
        uint32_t pc[AUD_DBG_LATE];
        uint16_t cnt[AUD_DBG_LATE];
        for (int i = 0; i < AUD_DBG_LATE; i++)
            pc[i] = aud_dbg_late_pc[i], cnt[i] = aud_dbg_late_cnt[i];
        aud_dbg_late_n = 0;
        irq_set_enabled(PWM_IRQ_WRAP_0, true);
        int n = snprintf(buf, buf_size, "AUDlate %lu", (unsigned long)ln);
        const uint32_t shown = ln < AUD_DBG_LATE ? ln : AUD_DBG_LATE;
        for (uint32_t k = 0; k < shown && n < (int)buf_size; k++)
        {
            const uint32_t i = (ln - shown + k) % AUD_DBG_LATE;
            n += snprintf(buf + n, buf_size - n, " %08lx/%u", (unsigned long)pc[i], cnt[i]);
        }
        if (n < (int)buf_size - 1)
            buf[n++] = '\n', buf[n] = 0;
        return 2;
    }
    if (state >= 2)
    {
        /* Enabled IRQs: number:priority:handler, a few per line. */
        int n = snprintf(buf, buf_size, "IRQ");
        int irq = state - 2;
        for (; irq < 32 && n < (int)buf_size - 24; irq++)
            if (irq_is_enabled(irq))
                n += snprintf(buf + n, buf_size - n, " %d:%02x:%08lx", irq, irq_get_priority(irq),
                              (unsigned long)irq_get_vtable_handler(irq));
        buf[n++] = '\n', buf[n] = 0;
        return irq < 32 ? irq + 2 : -1;
    }
    const uint32_t period = pwm_hw->slice[AUD_IRQ_SLICE].top + 1;
    irq_set_enabled(PWM_IRQ_WRAP_0, false);
    const uint32_t n = aud_dbg_n, sum = aud_dbg_sum, max = aud_dbg_max, over = aud_dbg_over,
                   skip = aud_dbg_skip;
    uint32_t prof[3];
    for (int i = 0; i < 3; i++)
        prof[i] = emu8950_prof[i], emu8950_prof[i] = 0;
    aud_dbg_n = aud_dbg_sum = aud_dbg_max = aud_dbg_over = aud_dbg_skip = 0;
    irq_set_enabled(PWM_IRQ_WRAP_0, true);
    const uint32_t avg = n ? sum / n : 0;
    const uint32_t ns = n ? n : 1;
    snprintf(buf, buf_size, "AUD dev=%d n=%lu avg=%lu max=%lu /%lu clk (%lu%%) over=%lu skip=%lu"
                            " opl pre=%lu slot=%lu ch=%lu\n",
             (int)aud_dev, (unsigned long)n, (unsigned long)avg, (unsigned long)max,
             (unsigned long)period, (unsigned long)(avg * 100 / period),
             (unsigned long)over, (unsigned long)skip, (unsigned long)(prof[0] / ns),
             (unsigned long)(prof[1] / ns), (unsigned long)(prof[2] / ns));
    return 1;
}

void aud_dbg_task(void)
{
    static uint32_t next;
    if (aud_dev == aud_dev_none || (int32_t)(time_us_32() - next) < 0)
        return;
    next = time_us_32() + 2000000;
    char buf[160];
    aud_status_response(buf, sizeof buf, 0, 0);
    printf("\r\n[%.*s]\r\n", (int)strcspn(buf, "\n"), buf);
}
#endif

void __in_flash("aud_init") aud_init(void)
{
    pwm_config config = pwm_get_default_config();
    pwm_config_set_wrap(&config, ((1u << AUD_PWM_BITS) - 1));
    pwm_init(AUD_L_SLICE, &config, true);
    pwm_init(AUD_R_SLICE, &config, true);
    pwm_init(AUD_IRQ_SLICE, &config, true);

    pwm_set_chan_level(AUD_L_SLICE, AUD_L_CHAN, AUD_PWM_CENTER);
    pwm_set_chan_level(AUD_R_SLICE, AUD_R_CHAN, AUD_PWM_CENTER);

    gpio_set_drive_strength(AUD_L_PIN, GPIO_DRIVE_STRENGTH_2MA);
    gpio_set_drive_strength(AUD_R_PIN, GPIO_DRIVE_STRENGTH_2MA);
    gpio_set_slew_rate(AUD_L_PIN, GPIO_SLEW_RATE_SLOW);
    gpio_set_slew_rate(AUD_R_PIN, GPIO_SLEW_RATE_SLOW);
    gpio_disable_pulls(AUD_L_PIN);
    gpio_disable_pulls(AUD_R_PIN);
    gpio_set_function(AUD_L_PIN, GPIO_FUNC_PWM);
    gpio_set_function(AUD_R_PIN, GPIO_FUNC_PWM);

    sine_init();
    bel_init();

    /* The IRQ slice has a period of 5149 system clocks, which is 49718 Hz and
     * 0.005% above AUD_NATIVE_RATE. The L and R pins are on other slices, so
     * their 250 kHz carrier does not change with the IRQ slice's period. */
    irq_set_priority(PWM_IRQ_WRAP_0, PICO_DEFAULT_IRQ_PRIORITY + 0x10);
    pwm_clear_irq(AUD_IRQ_SLICE);
#if PICO_RP2040
    irq_set_exclusive_handler(PWM_IRQ_WRAP_0, aud_irq_entry);
#else
    irq_set_exclusive_handler(PWM_IRQ_WRAP_0, aud_irq);
#endif
    pwm_set_wrap(AUD_IRQ_SLICE, (SYS_RP2350_KHZ * 1000u) / AUD_NATIVE_RATE - 1);
    pwm_set_irq_enabled(AUD_IRQ_SLICE, true);
    irq_set_enabled(PWM_IRQ_WRAP_0, true);
}

void aud_stop(void)
{
    aud_dev = aud_dev_none;
    ria_aud_watch(0xFFFF);
}

void aud_setup(aud_dev_t dev)
{
    aud_dev = dev;
}

/* Core1 queues rather than calling aud_xram_write, so the only thing that ever
 * reaches an engine here is aud_irq, and holding the engines still means
 * holding that off. Nothing on this side of the lock runs per write; it is
 * taken to reset a chip, which silences it anyway. */
void aud_engine_lock(void)
{
    irq_set_enabled(PWM_IRQ_WRAP_0, false);
    /* irq_set_enabled posts a write to the NVIC and adds no barrier of its
     * own, so without these the interrupt can still be taken after it
     * returns, which is the one thing this is here to stop. Letting go needs
     * no barrier: the interrupt coming back late costs nothing. */
    __dsb();
    __isb();
}

void aud_engine_unlock(void) { irq_set_enabled(PWM_IRQ_WRAP_0, true); }

aud_dev_t aud_device(void) { return aud_dev; }
