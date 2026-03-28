/*
 * serial.h — COM1 serial port debug output
 *
 * Serial output is the most reliable debug channel during early boot —
 * it works before the framebuffer is set up and its output can be
 * captured by QEMU's -serial stdio or -serial file:serial.log.
 *
 * We support:
 *   - 115200 baud, 8N1 (standard for QEMU)
 *   - Polled (non-interrupt-driven) output for Phase 1
 *   - Interrupt-driven output will be added in Phase 3 with the IRQ manager
 *
 * COM port addresses (ISA legacy):
 *   COM1: I/O base 0x3F8
 *   COM2: I/O base 0x2F8
 *   COM3: I/O base 0x3E8
 *   COM4: I/O base 0x2E8
 */

#pragma once

#include "../include/types.h"

#define SERIAL_COM1_BASE  0x3F8
#define SERIAL_COM2_BASE  0x2F8

/*
 * Initialize the serial port at the given I/O base address.
 * Configures 115200 baud, 8 data bits, no parity, 1 stop bit (8N1).
 * Returns true on success (loopback test passed), false if no port found.
 */
bool serial_init(uint16_t port);

/*
 * Write a single character to the serial port (busy-wait for TX ready).
 */
void serial_putc(char c);

/*
 * Write a null-terminated string.
 * Translates '\n' to '\r\n' for serial terminal compatibility.
 */
void serial_puts(const char *s);

/*
 * Write `len` bytes from `buf` (raw, no newline translation).
 */
void serial_write(const char *buf, size_t len);
