/*
 * ps2.c — i8042 PS/2 keyboard + mouse driver (polled)
 *
 * The 8042 has one shared output buffer; status-register bit 5 distinguishes
 * mouse bytes from keyboard bytes.  We disable controller IRQ generation and
 * poll the output buffer from the WM loop.  Keyboard bytes are set-1
 * scancodes (controller translation left enabled); mouse bytes form the
 * classic 3-byte movement packet.
 */

#include "../ke/spinlock.h"
#include "ps2.h"
#include "../arch/x86_64/cpu.h"
#include "../ke/printf.h"
#include "../wm/input.h"
#include "../lib/string.h"

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
static int  g_packet = 3;         /* 4 with a wheel (IntelliMouse) */
static bool g_explorer;           /* IntelliMouse Explorer: buttons 4 and 5, horizontal wheel */
static volatile bool g_ready;     /* ps2_init done: polling allowed */

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
    /* No controller (USB-only machines, or QEMU with i8042=off): the
     * status port floats to all ones */
    if (inb(PS2_STATUS) == 0xFF) {
        kprintf("[PS2] No controller\n");
        return false;
    }

    /* Disable both ports while we configure. */
    ctrl_cmd(CMD_DISABLE_P1);
    ctrl_cmd(CMD_DISABLE_P2);
    flush_output();

    /* Read config; disable both IRQs (we poll), translation on (bit6). */
    ctrl_cmd(CMD_READ_CFG);
    UINT8 cfg = read_data();
    cfg &= (UINT8)~0x03;     /* clear bit0 (kbd int), bit1 (mouse int) */
    cfg &= (UINT8)~0x10;     /* clear bit4 — ensure first port clock enabled */
    cfg &= (UINT8)~0x20;     /* clear bit5 — ensure mouse clock enabled */
    cfg |= 0x40;             /* translation on (set 1), which firmware may not have set after S3 */
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
    /* IntelliMouse: sample rates 200, 100, 80 in a row turn on the wheel;
     * the device then reports ID 3 and sends 4-byte packets */
    static const UINT8 knock[3] = { 200, 100, 80 };
    for (int i = 0; i < 3; i++) { mouse_cmd(0xF3); mouse_cmd(knock[i]); }
    if (mouse_cmd(0xF2) == 0xFA && read_data() == 3) {
        g_packet = 4;
        /* IntelliMouse Explorer: then 200, 200, 80 give ID 4, with the side
         * buttons and the horizontal wheel in the fourth byte */
        static const UINT8 knock2[3] = { 200, 200, 80 };
        for (int i = 0; i < 3; i++) { mouse_cmd(0xF3); mouse_cmd(knock2[i]); }
        if (mouse_cmd(0xF2) == 0xFA && read_data() == 4) g_explorer = true;
        mouse_cmd(0xF3); mouse_cmd(100);
    }
    UINT8 a2 = mouse_cmd(0xF4);    /* enable data reporting */
    g_have_mouse = (a1 == 0xFA || a2 == 0xFA);

    flush_output();
    kprintf("[PS2] Controller ready (keyboard + %s)\n",
            !g_have_mouse ? "no mouse" : g_explorer ? "5-button wheel mouse" : g_packet == 4 ? "wheel mouse" : "mouse");
    g_ready = true;
    return true;
}

/* After S3: the controller and devices were reset; set them up again */
void ps2_resume(void)
{
    g_ready = false;                     /* the timer's poll stays out meanwhile */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    ps2_init();
}

/* ---- decoding ---- */
static void handle_key(UINT8 sc)
{
    static bool e0;          /* previous byte was the E0 extended prefix */
    static int  e1_skip;     /* bytes left in a Pause (E1 ...) sequence */

    /* Controller/keyboard responses (ACK, resend, errors) are not keys */
    if (sc == 0xFA || sc == 0xFE || sc == 0x00 || sc == 0xFF) return;
    if (e1_skip > 0) { e1_skip--; return; }
    if (sc == 0xE1) { e1_skip = 5; return; }   /* Pause: not reported */
    if (sc == 0xE0) { e0 = true; return; }

    InputEvent ev;
    ev.type     = INPUT_KEY;
    ev.buttons  = 0;
    ev.dx = ev.dy = ev.dz = ev.dw = 0;
    ev.absolute = 0;
    ev.pressed  = (sc & 0x80) ? 0 : 1;   /* high bit set = break (release) */
    ev.scancode = (UINT8)(sc & 0x7F);
    ev.extended = e0 ? 1 : 0;
    e0 = false;
    InputPost(&ev);
}

static void handle_mouse_byte(UINT8 b)
{
    static UINT8 pkt[4];
    static int   idx;

    /* Resync: byte 0 always has bit 3 set. */
    if (idx == 0 && !(b & 0x08))
        return;

    pkt[idx++] = b;
    if (idx < g_packet) return;
    idx = 0;

    UINT8 flags = pkt[0];
    int dx = (int)pkt[1] - ((flags & 0x10) ? 256 : 0);
    int dy = (int)pkt[2] - ((flags & 0x20) ? 256 : 0);

    static UINT8 side;                      /* buttons 4 and 5 (Explorer packets) */
    InputEvent ev;
    memset(&ev, 0, sizeof(ev));             /* (from_pen and the rest stay 0: the PS/2 mouse is no pen) */
    ev.type     = INPUT_MOUSE;
    ev.scancode = 0;
    ev.pressed  = 0;
    ev.extended = 0;
    ev.absolute = 0;
    ev.dx       = dx;
    ev.dy       = -dy;                      /* PS/2 +y is up; screen +y down */
    ev.dz       = g_packet == 4 ? -(int)(INT8)pkt[3] : 0;   /* the device counts toward the user */
    ev.dw       = 0;
    if (g_explorer) {
        /* Fourth byte: bits 6-7 00 (or 11) wheel in bits 0-3 and buttons 4,
         * 5 in bits 4, 5; 01 a horizontal and 10 a vertical 6-bit wheel
         * (the buttons then stay as they were).  Counts are toward the
         * user and to the left. */
        UINT8 b = pkt[3];
        int six = (b & 0x20) ? (int)(b & 0x3F) - 64 : (int)(b & 0x3F);
        switch (b & 0xC0) {
        case 0x40: ev.dz = 0; ev.dw = -six; break;
        case 0x80: ev.dz = -six; break;
        default:
            ev.dz = -((b & 8) ? (int)(b & 0xF) - 16 : (int)(b & 0xF));
            side = (UINT8)(((b >> 4) & 3) << 3);
            break;
        }
    }
    ev.buttons  = (UINT8)((flags & 0x07) | side);   /* L|R|M, back, forward */
    InputPost(&ev);
}

/* Drain the controller.  Called from the timer interrupt (so input is
 * collected even while the desktop thread is busy drawing a frame and the
 * device's small buffer cannot overflow) and from the desktop loop.
 * A spinlock keeps the callers (on any CPU) from interleaving in the
 * packet/prefix decoders. */
static KSpinLock g_ps2_lock = KSPINLOCK_INIT;

void ps2_poll(void)
{
    if (!g_ready) return;
    IrqState irq = spin_lock_irqsave(&g_ps2_lock);
    for (int guard = 0; guard < 64; guard++) {
        UINT8 st = inb(PS2_STATUS);
        if (!(st & ST_OBF)) break;
        UINT8 data = inb(PS2_DATA);
        if (st & ST_AUX)
            handle_mouse_byte(data);
        else
            handle_key(data);
    }
    spin_unlock_irqrestore(&g_ps2_lock, irq);
}
