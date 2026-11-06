/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ria/mon/mon.h"
#include "core/str/rln.h"
#include "core/str/str.h"
#include "ria/sys/com.h"
#include "ria/sys/cfg.h"
#include "core/ria/regs.h"
#include "ria/sys/mbuf.h"
#include "ria/sys/pix.h"
#include "ria/sys/ria.h"
#include "ria/sys/vga.h"
#include "core/sys/config.h"
#include "ria.pio.h"
#include <pico/stdlib.h>
#include <hardware/clocks.h>
#include <hardware/dma.h>
#include <hardware/timer.h>
#include <strings.h>
#include <stdio.h>

// How long to wait for ACK to backchannel enable request
#define VGA_BACKCHANNEL_ACK_MS 3
// How long to wait before aborting version string
#define VGA_VERSION_WATCHDOG_MS 3
// Abandon backchannel after two missed vsync messages
// with 10ms added for UART drain and safety margin
// when changing canvas or timing.
#define VGA_VSYNC_WATCHDOG_MS 44

static enum {
    VGA_NOT_FOUND,       // Possibly normal, RP6502-VGA is optional
    VGA_TESTING,         // Looking for RP6502-VGA
    VGA_FOUND,           // Found
    VGA_CONNECTED,       // Connected and version string received
    VGA_NO_VERSION,      // Connected but no version string received
    VGA_CONNECTION_LOST, // Definitely an error condition
} vga_state;

static bool vga_needs_reset = true;
static vga_canvas_t vga_canvas_current = vga_canvas_console;
static volatile uint32_t vga_vsync_deadline;
static absolute_time_t vga_version_timer;
static uint8_t vga_vsync_frame;

static int vga_vsync_alarm = -1;
static uint vga_vsync_trap_offset;
static bool vga_vsync_alarm_arms;
static volatile bool vga_vsync_trap_armed;
static volatile bool vga_vsync_trap_fired;

#if PICO_RP2040
/* Pico 1 port bring-up: backchannel counters shown by STATUS. */
static volatile uint32_t vga_dbg_poll_bytes, vga_dbg_irq_bytes, vga_dbg_irq_calls;
static volatile uint32_t vga_dbg_trap_fires, vga_dbg_arms, vga_dbg_vsyncs;
static volatile int vga_dbg_lost_from = -1;
static uint vga_dbg_offset;
static uint32_t vga_dbg_ex_init, vga_dbg_ex_conn, vga_dbg_ins_conn;
#define VGA_DBG(x) (x)
#else
#define VGA_DBG(x) ((void)0)
#endif

#define VGA_VERSION_MESSAGE_SIZE 64
static char vga_version_message[VGA_VERSION_MESSAGE_SIZE];
static size_t vga_version_message_length;

static inline void vga_pix_backchannel_disable(void)
{
    pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x04, 0);
}

void vga_set_tel_console_active(bool active)
{
    pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x02, active ? 1 : 0);
}

void vga_set_code_page(uint16_t cp)
{
    pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x01, cp);
}
void vga_load_code_page(uint16_t cp)
{
    vga_set_code_page(cp);
}


static inline void vga_pix_backchannel_enable(void)
{
    pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x04, 1);
}

static inline void vga_pix_backchannel_request(void)
{
    pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x04, 2);
}

static inline void vga_backchannel_irq_enable(void)
{
    pio_set_irq0_source_enabled(VGA_BACKCHANNEL_PIO,
                                pis_sm0_rx_fifo_not_empty + VGA_BACKCHANNEL_SM, true);
}

static inline void vga_backchannel_irq_disable(void)
{
    pio_set_irq0_source_enabled(VGA_BACKCHANNEL_PIO,
                                pis_sm0_rx_fifo_not_empty + VGA_BACKCHANNEL_SM, false);
}

static void vga_vsync_trap_disarm(void)
{
    if (!vga_vsync_trap_armed)
        return;
    vga_vsync_trap_armed = false;
#if PICO_RP2040 && !PICO1_BC_SPLIT
    // The trap shares its SM with the receiver: mask it, don't reset the SM.
    pio_set_irq1_source_enabled(VGA_VSYNC_TRAP_PIO, pis_interrupt0, false);
#else
    pio_sm_exec(VGA_VSYNC_TRAP_PIO, VGA_VSYNC_TRAP_SM,
                pio_encode_jmp(vga_vsync_trap_offset));
    pio_sm_clear_fifos(VGA_VSYNC_TRAP_PIO, VGA_VSYNC_TRAP_SM);
#endif
    pio_interrupt_clear(VGA_VSYNC_TRAP_PIO, 0);
    // A trap interrupt that is already pending would otherwise run the handler
    // after the disarm and raise an extra VSYNC.
    irq_clear(VGA_VSYNC_TRAP_IRQ);
}

static void vga_vsync_alarm_handler(uint alarm)
{
    if (!vga_vsync_alarm_arms)
    {
        vga_vsync_trap_disarm();
        return;
    }
    // The trap is armed only after the version string phase ends, because the
    // start bit of a version byte would trigger it.
    if (vga_state != VGA_CONNECTED && vga_state != VGA_NO_VERSION)
        return;
    vga_vsync_trap_fired = false;
    vga_vsync_trap_armed = true;
    vga_vsync_alarm_arms = false;
    VGA_DBG(vga_dbg_arms++);
#if PICO_RP2040 && !PICO1_BC_SPLIT
    // Forget start bits from before the window; only the next one fires.
    pio_interrupt_clear(VGA_VSYNC_TRAP_PIO, 0);
    irq_clear(VGA_VSYNC_TRAP_IRQ);
    pio_set_irq1_source_enabled(VGA_VSYNC_TRAP_PIO, pis_interrupt0, true);
#else
    pio_sm_put(VGA_VSYNC_TRAP_PIO, VGA_VSYNC_TRAP_SM, 0);
#endif
    if (hardware_alarm_set_target(alarm, from_us_since_boot(
                                             time_us_64() + 2 * VGA_VSYNC_WINDOW_US)))
        vga_vsync_trap_disarm();
}

static void vga_vsync_reschedule(void)
{
    if (vga_vsync_alarm < 0)
        return;
    vga_vsync_trap_disarm();
    vga_vsync_alarm_arms = true;
    hardware_alarm_set_target((uint)vga_vsync_alarm,
                              from_us_since_boot(time_us_64() - VGA_VSYNC_TRANSIT_US +
                                                 VGA_VSYNC_PERIOD_US - VGA_VSYNC_WINDOW_US));
}

static void
    __attribute__((optimize("O3")))
    __isr
    __time_critical_func(vga_vsync_trap_irq_handler)(void)
{
#if PICO_RP2040 && !PICO1_BC_SPLIT
    pio_set_irq1_source_enabled(VGA_VSYNC_TRAP_PIO, pis_interrupt0, false);
#endif
    pio_interrupt_clear(VGA_VSYNC_TRAP_PIO, 0);
    vga_vsync_trap_armed = false;
    vga_vsync_trap_fired = true;
    VGA_DBG(vga_dbg_trap_fires++);
    // The frame number written here is a guess. The backchannel byte for this
    // VSYNC arrives about 74 us later, and when its frame number differs,
    // vga_backchannel_command writes the corrected number and calls
    // ria_trigger_vsync again.
    REGS(0xFFE3) = ++vga_vsync_frame;
    ria_trigger_vsync();
}

static inline void vga_backchannel_command(uint8_t byte)
{
    switch (byte & 0xF0)
    {
    case 0x80:
    {
        uint8_t frame = byte & 0xF;
        VGA_DBG(vga_dbg_vsyncs++);
        vga_vsync_deadline = time_us_32() + VGA_VSYNC_WATCHDOG_MS * 1000;
        vga_vsync_reschedule();
        if (vga_vsync_trap_fired)
        {
            vga_vsync_trap_fired = false;
            if ((vga_vsync_frame & 0xF) == frame)
                break;
        }
        if (frame < (vga_vsync_frame & 0xF))
            vga_vsync_frame += 0x10;
        vga_vsync_frame = (vga_vsync_frame & 0xF0) | frame;
        REGS(0xFFE3) = vga_vsync_frame;
        ria_trigger_vsync();
        break;
    }
    case 0x90:
        pix_ack();
        break;
    case 0xA0:
        pix_nak();
        break;
    }
}

static void
    __attribute__((optimize("O3")))
    __isr
    __time_critical_func(vga_backchannel_irq_handler)(void)
{
    VGA_DBG(vga_dbg_irq_calls++);
    while (!pio_sm_is_rx_fifo_empty(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM))
    {
        uint8_t byte = pio_sm_get(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM) >> 24;
        VGA_DBG(vga_dbg_irq_bytes++);
        if (byte & 0x80)
            vga_backchannel_command(byte);
    }
}

static void vga_rln_callback(bool timeout)
{
    // VGA1 means VGA on PIX 1
    if (!timeout && mbuf_len == 4 && !strncasecmp(STR_VGA1, (char *)mbuf, 4))
        vga_state = VGA_FOUND;
    else
        vga_state = VGA_NOT_FOUND;
}

static void vga_connect(void)
{
    VGA_DBG(vga_dbg_ex_conn = VGA_BACKCHANNEL_PIO->sm[VGA_BACKCHANNEL_SM].execctrl);
    VGA_DBG(vga_dbg_ins_conn = VGA_BACKCHANNEL_PIO->sm[VGA_BACKCHANNEL_SM].instr);
    vga_backchannel_irq_disable();
    while (stdio_getchar_timeout_us(0) != PICO_ERROR_TIMEOUT)
        tight_loop_contents();

    // Test if VGA connected
    mbuf_read(VGA_BACKCHANNEL_ACK_MS, vga_rln_callback, 4);
    vga_pix_backchannel_request();
    vga_state = VGA_TESTING;
    while (vga_state == VGA_TESTING)
        mbuf_task();
    if (vga_state == VGA_NOT_FOUND)
    {
        vga_pix_backchannel_disable();
        return;
    }

    // Turn on the backchannel
    pio_gpio_init(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_PIN);
    // Drain bytes the PIO sampled off the pin while it was driven by UART TX
    while (!pio_sm_is_rx_fifo_empty(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM))
        (void)pio_sm_get(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM);
    vga_pix_backchannel_enable();

    // Wait for version
    vga_version_message_length = 0;
    vga_version_timer = make_timeout_time_ms(VGA_VERSION_WATCHDOG_MS);
    while (true)
    {
        if (!pio_sm_is_rx_fifo_empty(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM))
        {
            uint8_t byte = pio_sm_get(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM) >> 24;
            VGA_DBG(vga_dbg_poll_bytes++);
            if (byte & 0x80)
            {
                vga_backchannel_command(byte);
            }
            else
            {
                vga_version_timer = make_timeout_time_ms(VGA_VERSION_WATCHDOG_MS);
                if (byte == '\r' || byte == '\n')
                {
                    if (vga_version_message_length > 0)
                    {
                        vga_state = VGA_CONNECTED;
                        break;
                    }
                }
                else if (vga_version_message_length < VGA_VERSION_MESSAGE_SIZE - 1u)
                {
                    vga_version_message[vga_version_message_length] = byte;
                    vga_version_message[++vga_version_message_length] = 0;
                }
            }
        }
        if (time_reached(vga_version_timer))
        {
            vga_state = VGA_NO_VERSION;
            break;
        }
    }

    // Enable backchannel IRQ now that version string phase is complete
    vga_backchannel_irq_enable();
    vga_vsync_deadline = time_us_32() + VGA_VSYNC_WATCHDOG_MS * 1000;
}

void __in_flash("vga_init") vga_init(void)
{
    // Disable backchannel for the case where RIA reboots and VGA doesn't
    vga_pix_backchannel_disable();
    vga_set_tel_console_active(false);

    // Program a UART Rx in PIO
    pio_sm_set_consecutive_pindirs(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM,
                                   VGA_BACKCHANNEL_PIN, 1, false);
    gpio_pull_up(VGA_BACKCHANNEL_PIN);
#if PICO_RP2040 && !PICO1_BC_SPLIT
    pio_sm_claim(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM); // keep it from CYW43
    int add = pio_add_program(VGA_BACKCHANNEL_PIO, &vga_backchannel_rx_trap_program);
    if (add < 0) // SDK 2.x returns an error instead of panicking
        panic("vga: no room in pio%u", pio_get_index(VGA_BACKCHANNEL_PIO));
    uint offset = (uint)add;
    pio_sm_config c = vga_backchannel_rx_trap_program_get_default_config(offset);
#else
#if PICO_RP2040 // PICO1_BC_SPLIT diagnostic: upstream layout, claimed
    pio_sm_claim(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM);
    pio_sm_claim(VGA_VSYNC_TRAP_PIO, VGA_VSYNC_TRAP_SM);
#endif
    uint offset = pio_add_program(VGA_BACKCHANNEL_PIO, &vga_backchannel_rx_program);
    pio_sm_config c = vga_backchannel_rx_program_get_default_config(offset);
#endif
    sm_config_set_in_pins(&c, VGA_BACKCHANNEL_PIN); // for WAIT, IN
    sm_config_set_in_shift(&c, true, true, 8);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    float div = (float)clock_get_hz(clk_sys) / (8 * VGA_BACKCHANNEL_BAUDRATE);
    sm_config_set_clkdiv(&c, div);
    VGA_DBG(vga_dbg_offset = offset);
    pio_sm_init(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM, offset, &c);
    pio_sm_set_enabled(VGA_BACKCHANNEL_PIO, VGA_BACKCHANNEL_SM, true);
    VGA_DBG(vga_dbg_ex_init = VGA_BACKCHANNEL_PIO->sm[VGA_BACKCHANNEL_SM].execctrl);

    // Set up PIO IRQ for backchannel RX, source stays disabled until connect
    irq_set_exclusive_handler(VGA_BACKCHANNEL_IRQ, vga_backchannel_irq_handler);
    irq_set_priority(VGA_BACKCHANNEL_IRQ, PICO_DEFAULT_IRQ_PRIORITY - 0x10);
    irq_set_enabled(VGA_BACKCHANNEL_IRQ, true);

#if PICO_RP2040 && !PICO1_BC_SPLIT
    // The trap is the receiver's IRQ flag 0, masked until armed.
    (void)vga_vsync_trap_offset;
    pio_interrupt_clear(VGA_VSYNC_TRAP_PIO, 0);
    pio_set_irq1_source_enabled(VGA_VSYNC_TRAP_PIO, pis_interrupt0, false);
#else
    vga_vsync_trap_offset = pio_add_program(VGA_VSYNC_TRAP_PIO, &vga_vsync_trap_program);
    pio_sm_config tc = vga_vsync_trap_program_get_default_config(vga_vsync_trap_offset);
    sm_config_set_in_pins(&tc, VGA_BACKCHANNEL_PIN); // for WAIT
    pio_sm_init(VGA_VSYNC_TRAP_PIO, VGA_VSYNC_TRAP_SM, vga_vsync_trap_offset, &tc);
    pio_sm_set_enabled(VGA_VSYNC_TRAP_PIO, VGA_VSYNC_TRAP_SM, true);
    pio_set_irq0_source_enabled(VGA_VSYNC_TRAP_PIO, pis_interrupt0, true);
#endif
    irq_set_exclusive_handler(VGA_VSYNC_TRAP_IRQ, vga_vsync_trap_irq_handler);
    irq_set_priority(VGA_VSYNC_TRAP_IRQ, PICO_DEFAULT_IRQ_PRIORITY - 0x10);
    irq_set_enabled(VGA_VSYNC_TRAP_IRQ, true);
    vga_vsync_alarm = (int)hardware_alarm_claim_unused(true);
    hardware_alarm_set_callback((uint)vga_vsync_alarm, vga_vsync_alarm_handler);
    // The alarm has the same priority as the backchannel and trap interrupts,
    // so none of the three handlers can preempt another in the middle of arming
    // or disarming the trap.
    irq_set_priority(hardware_alarm_get_irq_num((uint)vga_vsync_alarm),
                     PICO_DEFAULT_IRQ_PRIORITY - 0x10);

    // Re-send in case the first disable was lost
    vga_pix_backchannel_disable();

    // Connect and establish backchannel
    vga_connect();
}

void vga_task(void)
{
    if ((vga_state == VGA_CONNECTED || vga_state == VGA_NO_VERSION) &&
        (int32_t)(time_us_32() - vga_vsync_deadline) >= 0)
    {
        VGA_DBG(vga_dbg_lost_from = vga_state);
        vga_backchannel_irq_disable();
        vga_vsync_alarm_arms = false;
        if (vga_vsync_alarm >= 0)
            hardware_alarm_cancel((uint)vga_vsync_alarm);
        vga_vsync_trap_disarm();
        vga_pix_backchannel_disable();
        gpio_set_function(VGA_BACKCHANNEL_PIN, GPIO_FUNC_UART);
        vga_state = VGA_CONNECTION_LOST;
        mon_add_response_utf8(S(STR_ERR_VGA_CONNECTION_LOST));
    }

    if (vga_needs_reset)
    {
        vga_needs_reset = false;
        // VGA-side pix_ch15_xreg DISPLAY case calls vga_xreg_canvas(NULL),
        // which resets canvas to vga_canvas_console. Mirror that here.
        vga_canvas_current = vga_canvas_console;
        pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x00, vga_get_display_type());
    }
}

void vga_run(void)
{
    // It's normal to lose signal during Pico VGA development.
    // Attempt to restart when a 6502 program is run.
    if (vga_state == VGA_CONNECTION_LOST)
        vga_connect();
}

void vga_stop(void)
{
    vga_needs_reset = true;
}

void vga_break(void)
{
    vga_needs_reset = true;
}

vga_canvas_t vga_get_canvas(void)
{
    return vga_canvas_current;
}

void vga_canvas_size(int *w, int *h)
{
    vga_canvas_geometry(vga_canvas_current, w, h);
}

// The VGA draws the console on a 1280x1024 display as a 640x512 view, and on
// every other display as 640x480.
void vga_console_size(int *w, int *h)
{
    *w = 640;
    *h = vga_get_display_type() == 2 ? 512 : 480;
}

void vga_set_canvas(uint16_t canvas_word)
{
    // VGA side naks unknown canvas values and does nothing
    if (canvas_word < VGA_CANVAS_COUNT)
        vga_canvas_current = (vga_canvas_t)canvas_word;
}

bool vga_connected(void)
{
    return vga_state == VGA_CONNECTED ||
           vga_state == VGA_NO_VERSION;
}

int vga_boot_response(char *buf, size_t buf_size, int state, unsigned width)
{
    if (!vga_connected())
        return -1;
    return vga_status_response(buf, buf_size, state, width);
}

int vga_status_response(char *buf, size_t buf_size, int state, unsigned)
{
#if !PICO_RP2040
    (void)state;
#endif
    const char *msg = STR_VGA_SEARCHING;
    switch (vga_state)
    {
    case VGA_TESTING:
    case VGA_FOUND:
        break;
    case VGA_CONNECTED:
        msg = vga_version_message;
        break;
    case VGA_NO_VERSION:
        msg = STR_VGA_VERSION_UNKNOWN;
        break;
    case VGA_NOT_FOUND:
        msg = STR_VGA_NOT_FOUND;
        break;
    case VGA_CONNECTION_LOST:
        msg = STR_VGA_CONNECTION_LOST;
        break;
    }
#if PICO_RP2040
    PIO p = VGA_BACKCHANNEL_PIO;
    uint sm = VGA_BACKCHANNEL_SM;
    switch (state)
    {
    case 0:
        snprintf(buf, buf_size, "%s\n", msg);
        return 1;
    case 1:
        snprintf(buf, buf_size, "BC st=%d lost=%d poll=%lu irqc=%lu irqb=%lu vs=%lu arm=%lu trap=%lu\n",
                 (int)vga_state, vga_dbg_lost_from,
                 (unsigned long)vga_dbg_poll_bytes, (unsigned long)vga_dbg_irq_calls,
                 (unsigned long)vga_dbg_irq_bytes, (unsigned long)vga_dbg_vsyncs,
                 (unsigned long)vga_dbg_arms, (unsigned long)vga_dbg_trap_fires);
        return 2;
    default:
        snprintf(buf, buf_size, "BC pio%d sm%u pc=%u rx=%u pin=%d fn=%d f=%lx ie0=%lx ie1=%lx ir=%lx en=%lx nv=%d\n",
                 (int)pio_get_index(p), sm, (unsigned)pio_sm_get_pc(p, sm),
                 (unsigned)pio_sm_get_rx_fifo_level(p, sm),
                 (int)gpio_get(VGA_BACKCHANNEL_PIN), (int)gpio_get_function(VGA_BACKCHANNEL_PIN),
                 (unsigned long)p->irq, (unsigned long)p->inte0, (unsigned long)p->inte1,
                 (unsigned long)p->intr, (unsigned long)(p->ctrl & 0xF),
                 (int)irq_is_enabled(VGA_BACKCHANNEL_IRQ));
        return 3;
    case 3:
        snprintf(buf, buf_size, "BC ins=%lx dv=%lx ex=%lx pc=%lx sh=%lx fd=%lx by=%lx oe=%lx ck=%lu\n",
                 (unsigned long)p->sm[sm].instr, (unsigned long)p->sm[sm].clkdiv,
                 (unsigned long)p->sm[sm].execctrl, (unsigned long)p->sm[sm].pinctrl,
                 (unsigned long)p->sm[sm].shiftctrl, (unsigned long)p->fdebug,
                 (unsigned long)p->input_sync_bypass, (unsigned long)((p->dbg_padoe >> VGA_BACKCHANNEL_PIN) & 1),
                 (unsigned long)clock_get_hz(clk_sys));
        return 4;
    case 4:
        snprintf(buf, buf_size, "BC exi=%08lx exc=%08lx insc=%04lx off=%u\n",
                 (unsigned long)vga_dbg_ex_init, (unsigned long)vga_dbg_ex_conn,
                 (unsigned long)vga_dbg_ins_conn, (unsigned)vga_dbg_offset);
        return 5;
    case 5:
    case 6:
    {
        int n = snprintf(buf, buf_size, "DMA%c", state == 5 ? 'w' : 'r');
        for (uint ch = 0; ch < 12 && n < (int)buf_size - 10; ch++)
            n += snprintf(buf + n, buf_size - n, " %lx",
                          (unsigned long)(state == 5 ? dma_hw->ch[ch].write_addr
                                                     : dma_hw->ch[ch].read_addr));
        snprintf(buf + n, buf_size - n, "\n");
        return state == 5 ? 6 : -1;
    }
    }
#else
    snprintf(buf, buf_size, "%s\n", msg);
    return -1;
#endif
}

bool vga_check_display_type(uint8_t *v)
{
    return *v <= 2;
}

void vga_apply_display_type(uint8_t display_type, bool changed)
{
    (void)display_type;
    if (changed)
        vga_needs_reset = true;
}

int vga_display_type_response(char *buf, size_t buf_size, int state, unsigned width)
{
    (void)state;
    (void)width;
    snprintf(buf, buf_size, STR_SET_VGA_RESPONSE,
             vga_get_display_type(), vga_get_display_type_verbose());
    return -1;
}

const char *vga_get_display_type_verbose(void)
{
    const char *const labels[] = {STR_VGA_DISPLAY_TYPE_0,
                                  STR_VGA_DISPLAY_TYPE_1,
                                  STR_VGA_DISPLAY_TYPE_2};
    return labels[vga_get_display_type()];
}
