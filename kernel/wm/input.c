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
