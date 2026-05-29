/*
 * ps2.c — i8042 PS/2 keyboard + mouse driver (polled)
 *
 * The 8042 has one shared output buffer; status-register bit 5 distinguishes
 * mouse bytes from keyboard bytes.  We disable controller IRQ generation and
 * poll the output buffer from the WM loop.  Keyboard bytes are set-1
 * scancodes (controller translation left enabled); mouse bytes form the
 * classic 3-byte movement packet.
 */

#include "ps2.h"
#include "../arch/x86_64/cpu.h"
#include "../ke/printf.h"
#include "../wm/input.h"

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64

#define ST_OBF      0x01   /* output buffer full (data to read) */
#define ST_IBF      0x02   /* input buffer full (don't write yet) */
#define ST_AUX      0x20   /* byte came from the auxiliary (mouse) port */

/* Controller commands */
#define CMD_READ_CFG   0x20
#define CMD_WRITE_CFG  0x60
#define CMD_DISABLE_P2 0xA7
#define CMD_ENABLE_P2  0xA8
#define CMD_DISABLE_P1 0xAD
#define CMD_ENABLE_P1  0xAE
#define CMD_WRITE_P2   0xD4   /* next byte goes to the mouse */

static bool g_have_mouse;

/* ---- low-level helpers ---- */
static void wait_input_clear(void)
{
    for (int i = 0; i < 100000; i++)
        if (!(inb(PS2_STATUS) & ST_IBF)) return;
}

static void wait_output_full(void)
{
    for (int i = 0; i < 100000; i++)
        if (inb(PS2_STATUS) & ST_OBF) return;
}

static void ctrl_cmd(UINT8 c)        { wait_input_clear(); outb(PS2_CMD, c); }
static void write_data(UINT8 d)      { wait_input_clear(); outb(PS2_DATA, d); }
static UINT8 read_data(void)         { wait_output_full(); return inb(PS2_DATA); }

static void flush_output(void)
{
    for (int i = 0; i < 32 && (inb(PS2_STATUS) & ST_OBF); i++)
        (void)inb(PS2_DATA);
}

/* Send a command byte to the mouse and read its ACK (0xFA). */
static UINT8 mouse_cmd(UINT8 c)
{
    ctrl_cmd(CMD_WRITE_P2);
    write_data(c);
    return read_data();
}

bool ps2_init(void)
{
    /* Disable both ports while we configure. */
    ctrl_cmd(CMD_DISABLE_P1);
    ctrl_cmd(CMD_DISABLE_P2);
    flush_output();

    /* Read config; disable both IRQs (we poll), keep translation (bit6). */
    ctrl_cmd(CMD_READ_CFG);
    UINT8 cfg = read_data();
    cfg &= (UINT8)~0x03;     /* clear bit0 (kbd int), bit1 (mouse int) */
    cfg &= (UINT8)~0x10;     /* clear bit4 — ensure first port clock enabled */
    cfg &= (UINT8)~0x20;     /* clear bit5 — ensure mouse clock enabled */
    ctrl_cmd(CMD_WRITE_CFG);
    write_data(cfg);

    /* Enable both ports. */
    ctrl_cmd(CMD_ENABLE_P1);
    ctrl_cmd(CMD_ENABLE_P2);

    /* Keyboard: enable scanning. */
    write_data(0xF4);
    flush_output();

    /* Mouse: defaults + enable streaming reporting.  0xFA = ACK. */
    UINT8 a1 = mouse_cmd(0xF6);    /* set defaults */
    UINT8 a2 = mouse_cmd(0xF4);    /* enable data reporting */
    g_have_mouse = (a1 == 0xFA || a2 == 0xFA);

    flush_output();
    kprintf("[PS2] Controller ready (keyboard + %s)\n",
            g_have_mouse ? "mouse" : "no mouse");
    return true;
}

/* ---- decoding ---- */
static void handle_key(UINT8 sc)
{
    InputEvent ev;
    ev.type     = INPUT_KEY;
    ev.buttons  = 0;
    ev.dx = ev.dy = 0;
    ev.pressed  = (sc & 0x80) ? 0 : 1;   /* high bit set = break (release) */
    ev.scancode = (UINT8)(sc & 0x7F);
    InputPost(&ev);
}

static void handle_mouse_byte(UINT8 b)
{
    static UINT8 pkt[3];
    static int   idx;

    /* Resync: byte 0 always has bit 3 set. */
    if (idx == 0 && !(b & 0x08))
        return;

    pkt[idx++] = b;
    if (idx < 3) return;
    idx = 0;

    UINT8 flags = pkt[0];
    int dx = (int)pkt[1] - ((flags & 0x10) ? 256 : 0);
    int dy = (int)pkt[2] - ((flags & 0x20) ? 256 : 0);

    InputEvent ev;
    ev.type     = INPUT_MOUSE;
    ev.scancode = 0;
    ev.pressed  = 0;
    ev.buttons  = (UINT8)(flags & 0x07);   /* L|R|M */
    ev.dx       = dx;
    ev.dy       = -dy;                      /* PS/2 +y is up; screen +y down */
    InputPost(&ev);
}

void ps2_poll(void)
{
    for (int guard = 0; guard < 64; guard++) {
        UINT8 st = inb(PS2_STATUS);
        if (!(st & ST_OBF)) return;
        UINT8 data = inb(PS2_DATA);
        if (st & ST_AUX)
            handle_mouse_byte(data);
        else
            handle_key(data);
    }
}
