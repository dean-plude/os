/*
 * input.c — input event ring buffer
 *
 * Producers (PS/2 IRQ/poll) call InputPost; the WM event loop calls
 * InputPoll.  Head/tail are updated with atomics so the queue is safe even
 * once input moves to a real interrupt handler in a later phase.
 */

#include "input.h"
#include "../lib/string.h"

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

void InputPost(const InputEvent *ev)
{
    if (!ev) return;
    UINT32 head = __atomic_load_n(&g_head, __ATOMIC_ACQUIRE);
    UINT32 tail = __atomic_load_n(&g_tail, __ATOMIC_ACQUIRE);
    UINT32 next = (head + 1) & (INPUT_QUEUE_SIZE - 1);
    if (next == (tail & (INPUT_QUEUE_SIZE - 1)))
        return;   /* full — drop */
    g_queue[head & (INPUT_QUEUE_SIZE - 1)] = *ev;
    __atomic_store_n(&g_head, next, __ATOMIC_RELEASE);
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
 * Keyboard translation (US layout, scancode set 1)
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

static bool g_lshift, g_rshift, g_ctrl, g_alt, g_caps;

UINT32 InputModifiers(void)
{
    return (g_lshift || g_rshift ? 1u : 0u) | (g_ctrl ? 2u : 0u) | (g_alt ? 4u : 0u) | (g_caps ? 8u : 0u);
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
    }
    if (sc == KEY_CTRL) g_ctrl = down;       /* left or right */
    if (sc == KEY_ALT)  g_alt  = down;

    out->scancode = sc;
    out->extended = ev->extended != 0;
    out->pressed  = down;
    out->shift    = g_lshift || g_rshift;
    out->ctrl     = g_ctrl;
    out->alt      = g_alt;
    out->ch       = 0;

    if (!ev->extended && sc < sizeof(g_keymap)) {
        char c = out->shift ? g_keymap_shift[sc] : g_keymap[sc];
        if (g_caps && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        else if (g_caps && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c == 27) c = 0;                   /* Esc: use the scancode */
        out->ch = c;
    }
    return true;
}
