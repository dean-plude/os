/*
 * tablet.c — pen tablets: devices present and the packet ring (tablet.h)
 */

#include "tablet.h"
#include "../ke/spinlock.h"
#include "../ke/waitq.h"
#include "../ke/scheduler.h"
#include "../lib/string.h"

#define MAX_SYNTH 16

static KSpinLock    g_lock = KSPINLOCK_INIT;
static WaitQueue    g_q = WAITQ_INIT;
static TabletPacket g_ring[TABLET_RING];
static UINT32       g_serial;                 /* the newest packet's number */
static int          g_hw;                     /* hardware pens */
static struct { void *owner; int n; } g_synth[MAX_SYNTH];

void TabletDevice(void *owner, int delta)
{
    IrqState s = spin_lock_irqsave(&g_lock);
    if (!owner) g_hw = g_hw + delta < 0 ? 0 : g_hw + delta;
    else {
        int k = -1;
        for (int i = 0; i < MAX_SYNTH; i++) if (g_synth[i].owner == owner) { k = i; break; }
        if (k < 0 && delta > 0)
            for (int i = 0; i < MAX_SYNTH; i++) if (!g_synth[i].owner) { k = i; g_synth[i].owner = owner; break; }
        if (k >= 0) {
            g_synth[k].n += delta;
            if (g_synth[k].n <= 0) { g_synth[k].owner = NULL; g_synth[k].n = 0; }
        }
    }
    spin_unlock_irqrestore(&g_lock, s);
}

void TabletOwnerGone(void *owner)
{
    IrqState s = spin_lock_irqsave(&g_lock);
    for (int i = 0; i < MAX_SYNTH; i++)
        if (owner && g_synth[i].owner == owner) { g_synth[i].owner = NULL; g_synth[i].n = 0; }
    spin_unlock_irqrestore(&g_lock, s);
}

bool TabletOwns(void *owner)
{
    IrqState s = spin_lock_irqsave(&g_lock);
    bool r = false;
    for (int i = 0; i < MAX_SYNTH; i++) if (owner && g_synth[i].owner == owner && g_synth[i].n > 0) r = true;
    spin_unlock_irqrestore(&g_lock, s);
    return r;
}

int TabletDevices(void)
{
    IrqState s = spin_lock_irqsave(&g_lock);
    int n = g_hw;
    for (int i = 0; i < MAX_SYNTH; i++) n += g_synth[i].n;
    spin_unlock_irqrestore(&g_lock, s);
    return n;
}

void TabletPacketIn(const InputEvent *ev)
{
    TabletPacket p;
    p.time = (UINT32)(sched_ticks() * 10);
    p.x = ev->dx < 0 ? 0 : ev->dx > 65535 ? 65535 : ev->dx;
    p.y = ev->dy < 0 ? 0 : ev->dy > 65535 ? 65535 : ev->dy;
    p.pressure = ev->pressure > TABLET_PRESSURE ? TABLET_PRESSURE : ev->pressure;
    p.buttons = ev->buttons & 7;
    p.flags = (ev->pressed ? TABLET_INRANGE : 0) | (ev->extended ? TABLET_ERASER : 0);
    IrqState s = spin_lock_irqsave(&g_lock);
    p.serial = ++g_serial;
    g_ring[p.serial % TABLET_RING] = p;
    spin_unlock_irqrestore(&g_lock, s);
    waitq_wake(&g_q);
}

int TabletRead(UINT32 after, TabletPacket *out, int max, UINT64 wait_ticks, UINT32 *newest)
{
    UINT64 until = sched_ticks() + wait_ticks;
    for (;;) {
        UINT32 gen = waitq_gen(&g_q);
        int n = 0;
        IrqState s = spin_lock_irqsave(&g_lock);
        if (after < g_serial) {
            UINT32 first = g_serial >= TABLET_RING ? g_serial - TABLET_RING + 1 : 1;
            if (after + 1 > first) first = after + 1;
            for (UINT32 k = first; k <= g_serial && n < max; k++) out[n++] = g_ring[k % TABLET_RING];
        }
        if (newest) *newest = g_serial;
        spin_unlock_irqrestore(&g_lock, s);
        UINT64 now = sched_ticks();
        if (n || !max || now >= until) return n;
        waitq_wait(&g_q, gen, until - now);
    }
}
