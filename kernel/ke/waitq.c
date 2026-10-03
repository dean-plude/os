/*
 * waitq.c — wait queues (see waitq.h)
 */
#include "waitq.h"
#include "scheduler.h"

struct WaitqEntry {
    Thread            *t;
    volatile UINT32    woken;
    UINT32             tag;
    WaitqEntry        *next;
};

void waitq_wait(WaitQueue *q, UINT32 gen, UINT64 max_ticks)
{
    waitq_wait_tag(q, gen, max_ticks, 0);
}

void waitq_wait_tag(WaitQueue *q, UINT32 gen, UINT64 max_ticks, UINT32 tag)
{
    WaitqEntry me = { sched_current(), 0, tag, NULL };
    IrqState s = spin_lock_irqsave(&q->lock);
    /* Count ourselves in before looking at the generation, and the waker
     * moves the generation before counting sleepers: one of the two sees
     * the other (both are sequentially consistent) */
    __atomic_add_fetch(&q->sleepers, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&q->gen, __ATOMIC_SEQ_CST) != gen) {
        __atomic_sub_fetch(&q->sleepers, 1, __ATOMIC_SEQ_CST);
        spin_unlock_irqrestore(&q->lock, s);
        return;
    }
    me.next = q->list;
    q->list = &me;
    spin_unlock_irqrestore(&q->lock, s);

    sched_sleep_until(&me.woken, sched_ticks() + (max_ticks ? max_ticks : 1));

    s = spin_lock_irqsave(&q->lock);
    for (WaitqEntry **pp = &q->list; *pp; pp = &(*pp)->next)
        if (*pp == &me) { *pp = me.next; break; }
    __atomic_sub_fetch(&q->sleepers, 1, __ATOMIC_SEQ_CST);
    spin_unlock_irqrestore(&q->lock, s);
}

void waitq_wake(WaitQueue *q)
{
    waitq_wake_boost(q, BOOST_NONE, 0);
}

void waitq_wake_boost(WaitQueue *q, int boost, UINT32 tag)
{
    __atomic_add_fetch(&q->gen, 1, __ATOMIC_SEQ_CST);
    if (!__atomic_load_n(&q->sleepers, __ATOMIC_SEQ_CST)) return;
    IrqState s = spin_lock_irqsave(&q->lock);
    for (WaitqEntry *w = q->list; w; w = w->next) {
        if (w->woken) continue;
        w->woken = 1;
        sched_unblock_boost(w->t, !tag || w->tag == tag ? boost : BOOST_NONE);
    }
    spin_unlock_irqrestore(&q->lock, s);
}
