/*
 * serial.c — COM1 serial port driver (polled)
 *
 * 16550A UART register layout (offsets from I/O base):
 *   +0: RBR (read) / THR (write) — Receiver Buffer / Transmitter Holding
 *   +1: IER — Interrupt Enable Register
 *   +2: IIR (read) / FCR (write) — Interrupt ID / FIFO Control
 *   +3: LCR — Line Control Register
 *   +4: MCR — Modem Control Register
 *   +5: LSR — Line Status Register
 *   +6: MSR — Modem Status Register
 *   +7: SCR — Scratch Register
 *
 * When DLAB (Divisor Latch Access Bit, LCR bit 7) is set:
 *   +0: DLL — Divisor Latch Low byte
 *   +1: DLH — Divisor Latch High byte
 *
 * Baud rate divisor = 115200 / desired_baud
 * For 115200 baud: divisor = 1 (0x0001)
 */

#include "serial.h"
#include "../arch/x86_64/cpu.h"

/* Register offsets */
#define UART_RBR   0   /* Receive Buffer (DLAB=0, read) */
#define UART_THR   0   /* Transmit Holding (DLAB=0, write) */
#define UART_IER   1   /* Interrupt Enable (DLAB=0) */
#define UART_DLL   0   /* Divisor Latch Low (DLAB=1) */
#define UART_DLH   1   /* Divisor Latch High (DLAB=1) */
#define UART_IIR   2   /* Interrupt ID (read) */
#define UART_FCR   2   /* FIFO Control (write) */
#define UART_LCR   3   /* Line Control */
#define UART_MCR   4   /* Modem Control */
#define UART_LSR   5   /* Line Status */
#define UART_MSR   6   /* Modem Status */
#define UART_SCR   7   /* Scratch */

/* LSR bits */
#define LSR_DR    0x01  /* Data Ready (receive) */
#define LSR_THRE  0x20  /* Transmitter Holding Register Empty */

/* LCR bits */
#define LCR_DLAB  0x80  /* Divisor Latch Access Bit */
#define LCR_8N1   0x03  /* 8 data bits, no parity, 1 stop bit */

/* Active port base address */
static uint16_t g_serial_port;

bool serial_init(uint16_t port)
{
    /* Disable all interrupts */
    outb(port + UART_IER, 0x00);

    /* Enable DLAB and set baud rate to 115200 (divisor = 1) */
    outb(port + UART_LCR, LCR_DLAB);
    outb(port + UART_DLL, 0x01);   /* divisor low  = 1 */
    outb(port + UART_DLH, 0x00);   /* divisor high = 0 */

    /* Set 8N1, disable DLAB */
    outb(port + UART_LCR, LCR_8N1);

    /* Enable and reset FIFOs (14-byte trigger level) */
    outb(port + UART_FCR, 0xC7);

    /* Set RTS + DTR (IRQs disabled in MCR for polled mode) */
    outb(port + UART_MCR, 0x03);

    /* Loopback test: send 0xAE and check we receive it back */
    outb(port + UART_MCR, 0x1E);   /* enable loopback */
    outb(port + UART_THR, 0xAE);

    /* Wait briefly for the byte to loop back */
    for (int i = 0; i < 1000; i++) io_wait();

    if (!(inb(port + UART_LSR) & LSR_DR)) {
        /* No data received — port may not exist */
        outb(port + UART_MCR, 0x03);
        return false;
    }

    uint8_t received = inb(port + UART_RBR);
    if (received != 0xAE) {
        outb(port + UART_MCR, 0x03);
        return false;
    }

    /* Disable loopback, enable normal operation */
    outb(port + UART_MCR, 0x0F);   /* OUT1 + OUT2 + RTS + DTR */

    g_serial_port = port;
    return true;
}

/* Busy-wait until the UART's transmit holding register is empty */
static void serial_wait_tx(void)
{
    while (!(inb(g_serial_port + UART_LSR) & LSR_THRE))
        pause_cpu();
}

void serial_putc(char c)
{
    if (!g_serial_port) return;
    serial_wait_tx();
    outb(g_serial_port + UART_THR, (uint8_t)c);
}

void serial_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}

void serial_write(const char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        serial_putc(buf[i]);
    }
}
