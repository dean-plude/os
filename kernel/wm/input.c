/*
 * input.c — input event ring buffer
 *
 * Producers (PS/2 IRQ/poll, USB and virtio input, injected pen input) call
 * InputPost; the WM event loop calls InputPoll.  Head/tail are updated with atomics so the queue is safe even
 * once input moves to a real interrupt handler in a later phase.
 */

#include "input.h"
#include "../lib/string.h"
#include "../um/um.h"
#include "../ke/spinlock.h"
#include "kbdlayout.h"

#define INPUT_QUEUE_SIZE 256   /* must be a power of two */

static InputEvent g_queue[INPUT_QUEUE_SIZE];
static volatile UINT32 g_head;   /* next write slot */
static volatile UINT32 g_tail;   /* next read slot */

void InputInit(void)
{
    memset(g_queue, 0, sizeof(g_queue));
    g_head = 0;
    g_tail = 0;
}

/* Producers are many (the PS/2 and virtio polls on the desktop thread, USB
 * drivers' threads, programs injecting pen input): they take turns */
static KSpinLock g_post_lock = KSPINLOCK_INIT;

void InputPost(const InputEvent *ev)
{
    if (!ev) return;
    IrqState s = spin_lock_irqsave(&g_post_lock);
    UINT32 head = __atomic_load_n(&g_head, __ATOMIC_ACQUIRE);
    UINT32 tail = __atomic_load_n(&g_tail, __ATOMIC_ACQUIRE);
    UINT32 next = (head + 1) & (INPUT_QUEUE_SIZE - 1);
    if (next != (tail & (INPUT_QUEUE_SIZE - 1))) {          /* (full: dropped) */
        g_queue[head & (INPUT_QUEUE_SIZE - 1)] = *ev;
        __atomic_store_n(&g_head, next, __ATOMIC_RELEASE);
    }
    spin_unlock_irqrestore(&g_post_lock, s);
}

bool InputPoll(InputEvent *out)
{
    UINT32 head = __atomic_load_n(&g_head, __ATOMIC_ACQUIRE);
    UINT32 tail = __atomic_load_n(&g_tail, __ATOMIC_ACQUIRE);
    if ((tail & (INPUT_QUEUE_SIZE - 1)) == (head & (INPUT_QUEUE_SIZE - 1)))
        return false;   /* empty */
    if (out) *out = g_queue[tail & (INPUT_QUEUE_SIZE - 1)];
    __atomic_store_n(&g_tail, (tail + 1) & (INPUT_QUEUE_SIZE - 1),
                     __ATOMIC_RELEASE);
    return true;
}

/* -----------------------------------------------------------------------
 * Keyboard translation (scancode set 1): the keys every layout shares
 * here (Esc, Backspace, Tab, Enter, Space, keypad *), the typing keys
 * from the user's layout (kbdlayout.c)
 * ----------------------------------------------------------------------- */
static const char g_keymap[0x3A] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n', 0, 'a','s',
    'd','f','g','h','j','k','l',';','\'','`', 0, '\\','z','x','c','v',
    'b','n','m',',','.','/', 0, '*', 0, ' ',
};
static const char g_keymap_shift[0x3A] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n', 0, 'A','S',
    'D','F','G','H','J','K','L',':','"','~', 0, '|','Z','X','C','V',
    'B','N','M','<','>','?', 0, '*', 0, ' ',
};

static bool g_lshift, g_rshift, g_ctrl, g_alt, g_ralt, g_caps, g_num, g_scroll;
static UINT16 g_dead;                       /* a dead key's accent, waiting for its letter */

UINT32 InputModifiers(void)
{
    return (g_lshift || g_rshift ? 1u : 0u) | (g_ctrl ? 2u : 0u) | (g_alt ? 4u : 0u) | (g_caps ? 8u : 0u);
}

static int g_touch_contacts;

void InputTouchScreen(int contacts)
{
    if (contacts > TOUCH_MAX) contacts = TOUCH_MAX;
    if (contacts > g_touch_contacts) g_touch_contacts = contacts;
}

int InputTouchContacts(void) { return g_touch_contacts; }

UINT32 InputLockState(void)
{
    return (g_num ? 1u : 0u) | (g_caps ? 2u : 0u) | (g_scroll ? 4u : 0u);
}

bool InputTranslateKey(const InputEvent *ev, KeyEvent *out)
{
    if (!ev || !out || ev->type != INPUT_KEY) return false;
    UINT8 sc = ev->scancode;
    bool  down = ev->pressed != 0;

    /* Modifiers (E0 2A / E0 36 are fake shifts from Print Screen: ignore) */
    if (!ev->extended) {
        if (sc == KEY_LSHIFT) g_lshift = down;
        if (sc == KEY_RSHIFT) g_rshift = down;
        if (sc == KEY_CAPSLOCK && down) g_caps = !g_caps;
        if (sc == KEY_NUMLOCK && down) g_num = !g_num;
        if (sc == KEY_SCROLLLOCK && down) g_scroll = !g_scroll;
    }
    if (sc == KEY_CTRL) g_ctrl = down;       /* left or right */
    if (sc == KEY_ALT)  g_alt  = down;
    if (sc == KEY_ALT && ev->extended) g_ralt = down;
    /* Ctrl+Alt+F12: where every program's threads are (serial log), for a
     * program that hangs */
    if (sc == KEY_F12 && !ev->extended && down && g_ctrl && g_alt) UmDumpAll();

    out->scancode = sc;
    out->extended = ev->extended != 0;
    out->pressed  = down;
    out->shift    = g_lshift || g_rshift;
    out->ctrl     = g_ctrl;
    out->alt      = g_alt;
    out->altgr    = KbdHasAltGr() && (g_ralt || (g_ctrl && g_alt));
    out->ch       = 0;
    out->wch      = 0;

    UINT16 c = 0, lc;
    bool dead;
    if (!ev->extended && sc < sizeof(g_keymap)) {
        c = (UINT8)(out->shift ? g_keymap_shift[sc] : g_keymap[sc]);
        if (c == 27) c = 0;                   /* Esc: use the scancode */
    }
    if (!ev->extended && KbdKeyChar(sc, (out->shift ? 1 : 0) + (out->altgr ? 2 : 0), g_caps, &lc, &dead)) {
        c = lc;
        if (dead && lc) {                     /* an accent: kept for the next letter */
            if (down) g_dead = g_dead == lc ? 0 : lc;
            c = 0;
            if (down && !g_dead) c = lc;      /* (the same accent twice: the accent itself) */
        } else if (down && lc && g_dead) {
            UINT16 a = KbdComposeChar(g_dead, lc);
            if (a) c = a;
            g_dead = 0;
        }
        if (out->altgr && c) out->ctrl = out->alt = false;   /* an AltGr character is typed as such */
    } else if (down && sc == KEY_SPACE && !ev->extended && g_dead) {
        c = g_dead;                           /* accent, space: the accent itself */
        g_dead = 0;
    }
    out->wch = c;
    out->ch  = c < 0x80 ? (char)c : 0;
    return true;
}
