/*
 * aml.c — the ACPI namespace: uACPI's host functions, the "acpi" thread,
 * batteries, AC adapters and power buttons
 *
 * uACPI (third_party/uacpi, MIT) interprets the DSDT and SSDTs.  This file
 * gives it memory, I/O ports, PCI configuration space, time, locks, the
 * SCI and a work queue.  The SCI goes through the I/O APIC to uACPI's
 * handler, which checks the event status registers and queues GPE methods
 * and Notify handlers; the "acpi" thread, woken by it, runs them.  It also
 * calls the handler itself once a second in case an edge went missing (or
 * every 100 ms when there is no I/O APIC to route the SCI).
 *
 * The thread also reads the lid and the thermal zones, routes PCI
 * interrupts from _PRT and arms the wake devices (_PRW) before S3.
 *
 * Battery readings are cached by that thread (every 5 s, and when the
 * firmware notifies a change), so system calls never run AML.
 */

#include "aml.h"
#include "acpi.h"
#include "pci.h"
#include "ioapic.h"
#include "../arch/x86_64/idt.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/apic.h"
#include "../ke/printf.h"
#include "../ke/scheduler.h"
#include "../ke/smp.h"
#include "../ke/spinlock.h"
#include "../mm/vmm.h"
#include "../lib/string.h"

#include <uacpi/uacpi.h>
#include <uacpi/event.h>
#include <uacpi/notify.h>
#include <uacpi/sleep.h>
#include <uacpi/utilities.h>
#include <uacpi/resources.h>
#include <uacpi/kernel_api.h>

extern uint64_t g_tsc_per_tick;          /* apic.c: TSC ticks per 10 ms */

static volatile bool g_ready;
static Thread       *g_thread;

/* -----------------------------------------------------------------------
 * Host functions (uacpi/kernel_api.h)
 * ----------------------------------------------------------------------- */

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out)
{
    UINT64 rsdp = AcpiRsdpAddress();
    if (!rsdp) return UACPI_STATUS_NOT_FOUND;
    *out = rsdp;
    return UACPI_STATUS_OK;
}

void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len)
{
    volatile void *va = PciMapPhysical(addr, len);
    return va ? (void *)va : UACPI_MAP_FAILED;
}

void uacpi_kernel_unmap(void *addr, uacpi_size len) { (void)addr; (void)len; }

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char *msg)
{
    const char *tag = level <= UACPI_LOG_ERROR ? "error: " : level == UACPI_LOG_WARN ? "warning: " : "";
    kprintf("[ACPI] %s%s", tag, msg);
}

/* PCI configuration space: segment 0 through the legacy ports (offsets
 * below 256) */
typedef struct { UINT8 bus, dev, func; } PciHandle;

uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address a, uacpi_handle *out)
{
    if (a.segment != 0) return UACPI_STATUS_UNIMPLEMENTED;
    PciHandle *h = kmalloc(sizeof(*h));
    if (!h) return UACPI_STATUS_OUT_OF_MEMORY;
    h->bus = a.bus; h->dev = a.device; h->func = a.function;
    *out = h;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_pci_device_close(uacpi_handle h) { kfree(h); }

static uacpi_status pci_read(uacpi_handle hd, uacpi_size off, int bytes, UINT32 *v)
{
    PciHandle *h = hd;
    if (off + (uacpi_size)bytes > 256) { *v = 0xFFFFFFFFu; return UACPI_STATUS_OK; }
    UINT32 d = PciRead32(h->bus, h->dev, h->func, (UINT8)off) >> ((off & 3) * 8);
    *v = bytes == 4 ? d : d & ((1u << (bytes * 8)) - 1);
    return UACPI_STATUS_OK;
}

static uacpi_status pci_write(uacpi_handle hd, uacpi_size off, int bytes, UINT32 v)
{
    PciHandle *h = hd;
    if (off + (uacpi_size)bytes > 256) return UACPI_STATUS_OK;
    if (bytes == 4) { PciWrite32(h->bus, h->dev, h->func, (UINT8)off, v); return UACPI_STATUS_OK; }
    UINT32 d = PciRead32(h->bus, h->dev, h->func, (UINT8)off);
    int sh = (int)(off & 3) * 8;
    UINT32 mask = ((1u << (bytes * 8)) - 1) << sh;
    PciWrite32(h->bus, h->dev, h->func, (UINT8)off, (d & ~mask) | ((v << sh) & mask));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read8(uacpi_handle h, uacpi_size o, uacpi_u8 *v)
{ UINT32 x; uacpi_status s = pci_read(h, o, 1, &x); *v = (uacpi_u8)x; return s; }
uacpi_status uacpi_kernel_pci_read16(uacpi_handle h, uacpi_size o, uacpi_u16 *v)
{ UINT32 x; uacpi_status s = pci_read(h, o, 2, &x); *v = (uacpi_u16)x; return s; }
uacpi_status uacpi_kernel_pci_read32(uacpi_handle h, uacpi_size o, uacpi_u32 *v)
{ return pci_read(h, o, 4, v); }
uacpi_status uacpi_kernel_pci_write8(uacpi_handle h, uacpi_size o, uacpi_u8 v)   { return pci_write(h, o, 1, v); }
uacpi_status uacpi_kernel_pci_write16(uacpi_handle h, uacpi_size o, uacpi_u16 v) { return pci_write(h, o, 2, v); }
uacpi_status uacpi_kernel_pci_write32(uacpi_handle h, uacpi_size o, uacpi_u32 v) { return pci_write(h, o, 4, v); }

/* I/O ports: the handle is the base port */
uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len, uacpi_handle *out)
{
    if (base + len > 0x10000) return UACPI_STATUS_INVALID_ARGUMENT;
    *out = (uacpi_handle)(uintptr_t)base;
    return UACPI_STATUS_OK;
}
void uacpi_kernel_io_unmap(uacpi_handle h) { (void)h; }

#define PORT(h, o) ((UINT16)((uintptr_t)(h) + (o)))
uacpi_status uacpi_kernel_io_read8(uacpi_handle h, uacpi_size o, uacpi_u8 *v)    { *v = inb(PORT(h, o)); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_read16(uacpi_handle h, uacpi_size o, uacpi_u16 *v)  { *v = inw(PORT(h, o)); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_read32(uacpi_handle h, uacpi_size o, uacpi_u32 *v)  { *v = inl(PORT(h, o)); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_write8(uacpi_handle h, uacpi_size o, uacpi_u8 v)    { outb(PORT(h, o), v); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_write16(uacpi_handle h, uacpi_size o, uacpi_u16 v)  { outw(PORT(h, o), v); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_write32(uacpi_handle h, uacpi_size o, uacpi_u32 v)  { outl(PORT(h, o), v); return UACPI_STATUS_OK; }

void *uacpi_kernel_alloc(uacpi_size size) { return kmalloc(size); }
void uacpi_kernel_free(void *mem) { if (mem) kfree(mem); }

uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void)
{
    UINT64 tpt = g_tsc_per_tick, t = rdtsc();
    if (!tpt) return sched_ticks() * 10000000ULL;
    return t / tpt * 10000000ULL + t % tpt * 10000000ULL / tpt;
}

void uacpi_kernel_stall(uacpi_u8 usec) { udelay(usec); }

static bool can_sleep(void) { return (read_rflags() & 0x200) != 0; }

void uacpi_kernel_sleep(uacpi_u64 msec)
{
    if (!can_sleep()) { udelay(msec * 1000); return; }
    UINT64 end = sched_ticks() + (msec + 9) / 10;
    while (sched_ticks() < end) sched_sleep_until(NULL, end);
}

/* Waits for the mutexes and events: give up the CPU while waiting,
 * when interrupts are on */
static bool wait_for(bool (*try_take)(void *), void *obj, uacpi_u16 timeout_ms)
{
    UINT64 start = uacpi_kernel_get_nanoseconds_since_boot();
    for (;;) {
        if (try_take(obj)) return true;
        if (timeout_ms == 0) return false;
        if (timeout_ms != 0xFFFF &&
            uacpi_kernel_get_nanoseconds_since_boot() - start >= (UINT64)timeout_ms * 1000000ULL)
            return false;
        if (can_sleep()) sched_yield();
        else pause_cpu();
    }
}

typedef struct { volatile UINT32 held; } AmlMutex;

static bool mutex_try(void *p)
{
    UINT32 zero = 0;
    return __atomic_compare_exchange_n(&((AmlMutex *)p)->held, &zero, 1, false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

uacpi_handle uacpi_kernel_create_mutex(void) { return kzalloc(sizeof(AmlMutex)); }
void uacpi_kernel_free_mutex(uacpi_handle m) { kfree(m); }

uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle m, uacpi_u16 timeout)
{
    return wait_for(mutex_try, m, timeout) ? UACPI_STATUS_OK : UACPI_STATUS_TIMEOUT;
}

void uacpi_kernel_release_mutex(uacpi_handle m)
{
    __atomic_store_n(&((AmlMutex *)m)->held, 0, __ATOMIC_RELEASE);
}

typedef struct { volatile UINT32 count; } AmlEvent;

static bool event_try(void *p)
{
    AmlEvent *e = p;
    UINT32 c = __atomic_load_n(&e->count, __ATOMIC_ACQUIRE);
    while (c) {
        if (__atomic_compare_exchange_n(&e->count, &c, c - 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return true;
    }
    return false;
}

uacpi_handle uacpi_kernel_create_event(void) { return kzalloc(sizeof(AmlEvent)); }
void uacpi_kernel_free_event(uacpi_handle e) { kfree(e); }
uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle e, uacpi_u16 timeout) { return wait_for(event_try, e, timeout); }
void uacpi_kernel_signal_event(uacpi_handle e) { __atomic_fetch_add(&((AmlEvent *)e)->count, 1, __ATOMIC_RELEASE); }
void uacpi_kernel_reset_event(uacpi_handle e) { __atomic_store_n(&((AmlEvent *)e)->count, 0, __ATOMIC_RELEASE); }

uacpi_thread_id uacpi_kernel_get_thread_id(void) { return (uacpi_thread_id)sched_current(); }

uacpi_interrupt_state uacpi_kernel_disable_interrupts(void) { return irq_save(); }
void uacpi_kernel_restore_interrupts(uacpi_interrupt_state s) { irq_restore(s); }

uacpi_handle uacpi_kernel_create_spinlock(void) { return kzalloc(sizeof(KSpinLock)); }
void uacpi_kernel_free_spinlock(uacpi_handle l) { kfree(l); }
uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle l) { return spin_lock_irqsave(l); }
void uacpi_kernel_unlock_spinlock(uacpi_handle l, uacpi_cpu_flags f) { spin_unlock_irqrestore(l, f); }

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *req)
{
    if (req->type == UACPI_FIRMWARE_REQUEST_TYPE_FATAL)
        kprintf("[ACPI] firmware reports a fatal error (type %u, code %u, argument %llu)\n",
                req->fatal.type, req->fatal.code, (unsigned long long)req->fatal.arg);
    return UACPI_STATUS_OK;
}

/* The SCI: an interrupt through the I/O APIC, else polled by the acpi thread */
static uacpi_interrupt_handler g_sci;
static uacpi_handle            g_sci_ctx;
static bool                    g_sci_irq;
static volatile UINT32         g_kick;           /* work queued, or something to read */
static volatile UINT32         g_work_busy;

static void kick(void)
{
    __atomic_store_n(&g_kick, 1, __ATOMIC_RELEASE);
    if (g_thread) sched_unblock(g_thread);
}

static void sci_interrupt(void *ctx)
{
    (void)ctx;
    if (!g_sci) return;
    g_sci(g_sci_ctx);                                /* (queuing work kicks the thread) */
}

uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 irq, uacpi_interrupt_handler h,
                                                    uacpi_handle ctx, uacpi_handle *out)
{
    g_sci_ctx = ctx;
    g_sci = h;
    *out = (uacpi_handle)&g_sci;
    UINT32 gsi;                                      /* (the SCI is shareable, level, active low) */
    if (IrqInstall(IRQ_SCI, sci_interrupt, NULL) && IoApicRouteIrq(irq, true, true, IRQ_SCI, &gsi)) {
        g_sci_irq = true;
        kprintf("[ACPI] SCI on IRQ %u (GSI %u)\n", irq, gsi);
    } else {
        IrqInstall(IRQ_SCI, NULL, NULL);
        kprintf("[ACPI] SCI on IRQ %u: no I/O APIC input, polled\n", irq);
    }
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler h, uacpi_handle irq)
{
    (void)h; (void)irq;
    g_sci = NULL;
    return UACPI_STATUS_OK;
}

bool AmlSciIsInterrupt(void) { return g_sci_irq; }

/* Work (GPE methods, Notify handlers): queued from the SCI handler, run
 * by the acpi thread */
#define WORK_MAX 64
typedef struct { uacpi_work_handler fn; uacpi_handle ctx; } Work;
static Work            g_work[WORK_MAX];
static UINT32          g_work_head, g_work_tail;
static KSpinLock       g_work_lock = KSPINLOCK_INIT;

uacpi_status uacpi_kernel_schedule_work(uacpi_work_type type, uacpi_work_handler fn, uacpi_handle ctx)
{
    (void)type;
    IrqState s = spin_lock_irqsave(&g_work_lock);
    if (g_work_tail - g_work_head >= WORK_MAX) {
        spin_unlock_irqrestore(&g_work_lock, s);
        return UACPI_STATUS_OUT_OF_MEMORY;
    }
    g_work[g_work_tail++ % WORK_MAX] = (Work){ fn, ctx };
    __atomic_fetch_add(&g_work_busy, 1, __ATOMIC_SEQ_CST);
    spin_unlock_irqrestore(&g_work_lock, s);
    kick();
    return UACPI_STATUS_OK;
}

static void run_work(void)
{
    for (;;) {
        IrqState s = spin_lock_irqsave(&g_work_lock);
        if (g_work_head == g_work_tail) { spin_unlock_irqrestore(&g_work_lock, s); return; }
        Work w = g_work[g_work_head++ % WORK_MAX];
        spin_unlock_irqrestore(&g_work_lock, s);
        w.fn(w.ctx);
        __atomic_fetch_sub(&g_work_busy, 1, __ATOMIC_SEQ_CST);
    }
}

uacpi_status uacpi_kernel_wait_for_work_completion(void)
{
    if (sched_current() == g_thread) { run_work(); return UACPI_STATUS_OK; }
    while (__atomic_load_n(&g_work_busy, __ATOMIC_ACQUIRE)) {
        if (can_sleep()) sched_wait();
        else pause_cpu();
    }
    return UACPI_STATUS_OK;
}

static void log_device(const char *what, uacpi_namespace_node *node)
{
    const uacpi_char *path = uacpi_namespace_node_generate_absolute_path(node);
    kprintf("[ACPI] %s %s\n", what, path ? path : "?");
    uacpi_free_absolute_path(path);
}

/* -----------------------------------------------------------------------
 * Power buttons
 * ----------------------------------------------------------------------- */
static volatile UINT32 g_pwrbtn;

static uacpi_interrupt_ret fixed_power_button(uacpi_handle ctx)
{
    (void)ctx;
    __atomic_store_n(&g_pwrbtn, 1, __ATOMIC_RELEASE);
    return UACPI_INTERRUPT_HANDLED;
}

static uacpi_status button_notify(uacpi_handle ctx, uacpi_namespace_node *node, uacpi_u64 value)
{
    (void)ctx; (void)node;
    if (value == 0x80) __atomic_store_n(&g_pwrbtn, 1, __ATOMIC_RELEASE);
    return UACPI_STATUS_OK;
}

static uacpi_iteration_decision found_button(void *user, uacpi_namespace_node *node, uacpi_u32 depth)
{
    (void)user; (void)depth;
    uacpi_install_notify_handler(node, button_notify, NULL);
    log_device("Power button", node);
    return UACPI_ITERATION_DECISION_CONTINUE;
}

bool AmlPowerButtonPressed(void)
{
    return __atomic_exchange_n(&g_pwrbtn, 0, __ATOMIC_ACQ_REL) != 0;
}

/* -----------------------------------------------------------------------
 * Batteries (PNP0C0A) and AC adapters (ACPI0003)
 * ----------------------------------------------------------------------- */
#define MAX_BATTERIES 4
#define MAX_ADAPTERS  4
#define UNKNOWN       0xFFFFFFFFu

typedef struct {
    uacpi_namespace_node *node;
    bool   present, have_info;
    UINT32 unit;                         /* 0 mWh / mW, 1 mAh / mA */
    UINT32 design_voltage;               /* mV */
    UINT32 full, warning, low;           /* capacities in the battery's unit */
} Battery;

static Battery               g_bat[MAX_BATTERIES];
static int                   g_nbat;
static uacpi_namespace_node *g_ac[MAX_ADAPTERS];
static int                   g_nac;
static volatile UINT32       g_refresh = 1;
static AmlBatteryState       g_state = { .ac_online = true, .estimated_time = UNKNOWN };
static KSpinLock             g_state_lock = KSPINLOCK_INIT;

static bool package_u32(uacpi_object_array *a, uacpi_size i, UINT32 *out)
{
    uacpi_u64 v;
    if (i >= a->count || uacpi_object_get_integer(a->objects[i], &v) != UACPI_STATUS_OK) return false;
    *out = (UINT32)v;
    return true;
}

/* _BIX (ACPI 4+) or _BIF: the unit, the last full charge and the alarms */
static void read_info(Battery *b)
{
    uacpi_object *o = NULL;
    uacpi_object_array a;
    int base;
    if (uacpi_eval_simple_package(b->node, "_BIX", &o) == UACPI_STATUS_OK) base = 1;
    else if (uacpi_eval_simple_package(b->node, "_BIF", &o) == UACPI_STATUS_OK) base = 0;
    else return;
    if (uacpi_object_get_package(o, &a) == UACPI_STATUS_OK) {
        UINT32 design = UNKNOWN;
        b->have_info = package_u32(&a, base + 0, &b->unit) &&
                       package_u32(&a, base + 1, &design) &&
                       package_u32(&a, base + 2, &b->full) &&
                       package_u32(&a, base + 4, &b->design_voltage) &&
                       package_u32(&a, base + 5, &b->warning) &&
                       package_u32(&a, base + 6, &b->low);
        if (b->have_info && (b->full == 0 || b->full == UNKNOWN)) b->full = design;
    }
    uacpi_object_unref(o);
}

/* A value in the battery's unit as mWh (or mW) */
static UINT32 to_mw(const Battery *b, UINT32 v)
{
    if (v == UNKNOWN) return UNKNOWN;
    if (b->unit == 0) return v;
    if (!b->design_voltage || b->design_voltage == UNKNOWN) return v;
    return (UINT32)((UINT64)v * b->design_voltage / 1000);
}

static void refresh(void)
{
    AmlBatteryState st = { .estimated_time = UNKNOWN };
    bool any_ac = false, ac = false;
    for (int i = 0; i < g_nac; i++) {
        uacpi_u64 psr;
        if (uacpi_eval_simple_integer(g_ac[i], "_PSR", &psr) != UACPI_STATUS_OK) continue;
        any_ac = true;
        if (psr) ac = true;
    }
    INT64 rate = 0;
    bool rate_known = true;
    for (int i = 0; i < g_nbat; i++) {
        Battery *b = &g_bat[i];
        uacpi_u32 sta = 0;
        bool present = uacpi_eval_sta(b->node, &sta) == UACPI_STATUS_OK && (sta & (1u << 4));
        if (present && (!b->present || !b->have_info)) read_info(b);
        b->present = present;
        if (!present || !b->have_info) continue;

        uacpi_object *o = NULL;
        uacpi_object_array a;
        UINT32 state, now, left;
        if (uacpi_eval_simple_package(b->node, "_BST", &o) != UACPI_STATUS_OK) continue;
        bool ok = uacpi_object_get_package(o, &a) == UACPI_STATUS_OK &&
                  package_u32(&a, 0, &state) && package_u32(&a, 1, &now) && package_u32(&a, 2, &left);
        uacpi_object_unref(o);
        if (!ok) continue;

        st.battery_present = true;
        if (state & 1) st.discharging = true;
        if (state & 2) st.charging = true;
        st.max_capacity += to_mw(b, b->full) == UNKNOWN ? 0 : to_mw(b, b->full);
        st.remaining_capacity += to_mw(b, left) == UNKNOWN ? 0 : to_mw(b, left);
        st.alert_low += to_mw(b, b->low) == UNKNOWN ? 0 : to_mw(b, b->low);
        st.alert_warning += to_mw(b, b->warning) == UNKNOWN ? 0 : to_mw(b, b->warning);
        if (to_mw(b, now) == UNKNOWN) rate_known = false;
        else if (state & 1) rate -= to_mw(b, now);
        else if (state & 2) rate += to_mw(b, now);
    }
    st.ac_online = any_ac ? ac : !st.discharging;
    st.rate = rate_known ? (INT32)rate : (INT32)0x80000000;
    if (st.discharging && rate_known && rate < 0)
        st.estimated_time = (UINT32)((UINT64)st.remaining_capacity * 3600 / (UINT64)(-rate));

    IrqState s = spin_lock_irqsave(&g_state_lock);
    g_state = st;
    spin_unlock_irqrestore(&g_state_lock, s);
}

void AmlGetBatteryState(AmlBatteryState *out)
{
    IrqState s = spin_lock_irqsave(&g_state_lock);
    *out = g_state;
    spin_unlock_irqrestore(&g_state_lock, s);
}

/* 0x80: status changed, 0x81: information changed (a battery came or went) */
static uacpi_status power_notify(uacpi_handle ctx, uacpi_namespace_node *node, uacpi_u64 value)
{
    (void)node;
    Battery *b = ctx;
    if (b && value == 0x81) b->have_info = false;
    __atomic_store_n(&g_refresh, 1, __ATOMIC_RELEASE);
    return UACPI_STATUS_OK;
}

static uacpi_iteration_decision found_battery(void *user, uacpi_namespace_node *node, uacpi_u32 depth)
{
    (void)user; (void)depth;
    if (g_nbat == MAX_BATTERIES) return UACPI_ITERATION_DECISION_BREAK;
    Battery *b = &g_bat[g_nbat++];
    b->node = node;
    uacpi_install_notify_handler(node, power_notify, b);
    log_device("Battery", node);
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_iteration_decision found_adapter(void *user, uacpi_namespace_node *node, uacpi_u32 depth)
{
    (void)user; (void)depth;
    if (g_nac == MAX_ADAPTERS) return UACPI_ITERATION_DECISION_BREAK;
    g_ac[g_nac++] = node;
    uacpi_install_notify_handler(node, power_notify, NULL);
    log_device("AC adapter", node);
    return UACPI_ITERATION_DECISION_CONTINUE;
}

/* -----------------------------------------------------------------------
 * The lid (PNP0C0D)
 * ----------------------------------------------------------------------- */
static uacpi_namespace_node *g_lid;
static volatile INT32        g_lid_closed = -1;  /* -1: not read yet */
static volatile UINT32       g_lid_read = 1, g_lid_event;

static void read_lid(void)
{
    uacpi_u64 v;
    if (!g_lid || uacpi_eval_simple_integer(g_lid, "_LID", &v) != UACPI_STATUS_OK) return;
    INT32 closed = v == 0, was = g_lid_closed;
    if (closed == was) return;
    g_lid_closed = closed;
    kprintf("[ACPI] Lid %s\n", closed ? "closed" : "open");
    if (closed && was == 0) __atomic_store_n(&g_lid_event, 1, __ATOMIC_RELEASE);
}

static uacpi_status lid_notify(uacpi_handle ctx, uacpi_namespace_node *node, uacpi_u64 value)
{
    (void)ctx; (void)node;
    if (value == 0x80) __atomic_store_n(&g_lid_read, 1, __ATOMIC_RELEASE);
    return UACPI_STATUS_OK;
}

static uacpi_iteration_decision found_lid(void *user, uacpi_namespace_node *node, uacpi_u32 depth)
{
    (void)user; (void)depth;
    g_lid = node;
    uacpi_install_notify_handler(node, lid_notify, NULL);
    log_device("Lid", node);
    return UACPI_ITERATION_DECISION_BREAK;
}

bool AmlLidPresent(void) { return g_lid != NULL; }
bool AmlLidClosed(void) { return g_lid_closed == 1; }
bool AmlLidClosedEvent(void) { return __atomic_exchange_n(&g_lid_event, 0, __ATOMIC_ACQ_REL) != 0; }

/* -----------------------------------------------------------------------
 * Thermal zones
 * ----------------------------------------------------------------------- */
#define MAX_ZONES 8

typedef struct {
    uacpi_namespace_node *node;
    AmlThermalZone        z;
    UINT64                next;              /* tick of the next reading */
    bool                  trips, hot_sent, crit_sent;
} Zone;

static Zone            g_zone[MAX_ZONES];
static int             g_nzone;
static volatile UINT32 g_zone_read;          /* a notification: read them now */
static volatile UINT32 g_thermal_req;
static KSpinLock       g_zone_lock = KSPINLOCK_INIT;

static UINT32 zone_int(Zone *zn, const char *name)
{
    uacpi_u64 v;
    return uacpi_eval_simple_integer(zn->node, name, &v) == UACPI_STATUS_OK && v > 0 && v < 0x10000 ? (UINT32)v : 0;
}

/* tenths of a kelvin as "40.0 C" */
static void celsius(char *out, UINT32 dk)
{
    INT32 t = (INT32)dk - 2732;
    const char *sign = t < 0 ? "-" : "";
    if (t < 0) t = -t;
    ksnprintf(out, 16, "%s%d.%d C", sign, t / 10, t % 10);
}

static void read_trips(Zone *zn)
{
    AmlThermalZone z = zn->z;
    z.passive = zone_int(zn, "_PSV");
    z.hot = zone_int(zn, "_HOT");
    z.critical = zone_int(zn, "_CRT");
    uacpi_u64 tzp = 0;
    z.period = uacpi_eval_simple_integer(zn->node, "_TZP", &tzp) == UACPI_STATUS_OK ? (UINT32)tzp : 0;
    IrqState s = spin_lock_irqsave(&g_zone_lock);
    zn->z = z;
    spin_unlock_irqrestore(&g_zone_lock, s);
    zn->trips = true;
}

static void read_zone(Zone *zn)
{
    if (!zn->trips) read_trips(zn);
    UINT32 t = zone_int(zn, "_TMP");
    /* polled every _TZP (at least every 10 s, as notifications alone can be missed) */
    UINT32 period = zn->z.period && zn->z.period < 100 ? zn->z.period : 100;
    zn->next = sched_ticks() + period * 10;
    if (!t) return;
    AmlThermalZone z = zn->z;
    bool cooling = z.passive && t >= z.passive;
    char now[16], trip[16];
    celsius(now, t);
    if (cooling != z.cooling) {
        celsius(trip, z.passive);
        kprintf("[ACPI] %s: %s, %s the passive trip point (%s): passive cooling %s\n", z.name, now,
                cooling ? "at or above" : "below", trip, cooling ? "on" : "off");
    }
    if (z.critical && t >= z.critical) {
        if (!zn->crit_sent) {
            celsius(trip, z.critical);
            kprintf("[ACPI] %s: %s reached the critical trip point (%s): shutting down\n", z.name, now, trip);
            __atomic_store_n(&g_thermal_req, AML_THERMAL_SHUTDOWN, __ATOMIC_RELEASE);
        }
        zn->crit_sent = true;
    } else if (z.hot && t >= z.hot) {
        if (!zn->hot_sent) {
            celsius(trip, z.hot);
            kprintf("[ACPI] %s: %s reached the hot trip point (%s): sleeping\n", z.name, now, trip);
            if (g_thermal_req == AML_THERMAL_NONE)
                __atomic_store_n(&g_thermal_req, AML_THERMAL_SLEEP, __ATOMIC_RELEASE);
        }
        zn->hot_sent = true;
    } else {
        zn->hot_sent = zn->crit_sent = false;
    }
    if (t != z.temp || cooling != z.cooling) {
        IrqState s = spin_lock_irqsave(&g_zone_lock);
        zn->z.temp = t;
        zn->z.cooling = cooling;
        zn->z.stamp++;
        spin_unlock_irqrestore(&g_zone_lock, s);
    }
}

/* 0x80: the temperature changed, 0x81: the trip points did */
static uacpi_status zone_notify(uacpi_handle ctx, uacpi_namespace_node *node, uacpi_u64 value)
{
    (void)node;
    Zone *zn = ctx;
    if (value == 0x81) zn->trips = false;
    zn->next = 0;
    __atomic_store_n(&g_zone_read, 1, __ATOMIC_RELEASE);
    return UACPI_STATUS_OK;
}

static uacpi_iteration_decision found_zone(void *user, uacpi_namespace_node *node, uacpi_u32 depth)
{
    (void)user; (void)depth;
    if (g_nzone == MAX_ZONES) return UACPI_ITERATION_DECISION_BREAK;
    Zone *zn = &g_zone[g_nzone];
    zn->node = node;
    const uacpi_char *path = uacpi_namespace_node_generate_absolute_path(node);
    strncpy(zn->z.name, path ? path : "?", sizeof(zn->z.name) - 1);
    uacpi_free_absolute_path(path);
    read_zone(zn);
    char t[16] = "?", p[16] = "none", h[16] = "none", c[16] = "none";
    if (zn->z.temp) celsius(t, zn->z.temp);
    if (zn->z.passive) celsius(p, zn->z.passive);
    if (zn->z.hot) celsius(h, zn->z.hot);
    if (zn->z.critical) celsius(c, zn->z.critical);
    kprintf("[ACPI] Thermal zone %s: %s (passive %s, hot %s, critical %s, polled every %u.%u s)\n",
            zn->z.name, t, p, h, c, (zn->z.period && zn->z.period < 100 ? zn->z.period : 100) / 10,
            (zn->z.period && zn->z.period < 100 ? zn->z.period : 100) % 10);
    uacpi_install_notify_handler(node, zone_notify, zn);
    g_nzone++;
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static void poll_zones(bool all)
{
    UINT64 now = sched_ticks();
    for (int i = 0; i < g_nzone; i++)
        if (all || now >= g_zone[i].next) read_zone(&g_zone[i]);
}

int AmlThermalZones(AmlThermalZone *out, int max)
{
    IrqState s = spin_lock_irqsave(&g_zone_lock);
    int n = g_nzone < max ? g_nzone : max;
    for (int i = 0; i < n; i++) out[i] = g_zone[i].z;
    spin_unlock_irqrestore(&g_zone_lock, s);
    return n;
}

int AmlThermalRequest(void) { return (int)__atomic_exchange_n(&g_thermal_req, AML_THERMAL_NONE, __ATOMIC_ACQ_REL); }

/* -----------------------------------------------------------------------
 * PCI interrupt routing: the root bridge's _PRT (APIC mode, \_PIC(1)),
 * with link devices (PNP0C0F) resolved through their _CRS
 * ----------------------------------------------------------------------- */
#define MAX_ROUTES 128

typedef struct { UINT8 dev, pin; bool level, low; UINT32 gsi; } PciRoute;
static PciRoute g_prt[MAX_ROUTES];
static int      g_nprt;

typedef struct { UINT32 gsi; bool level, low, found; } LinkIrq;

static uacpi_iteration_decision link_resource(void *user, uacpi_resource *r)
{
    LinkIrq *l = user;
    if (r->type == UACPI_RESOURCE_TYPE_IRQ && r->irq.num_irqs) {
        l->gsi = r->irq.irqs[0];
        l->level = r->irq.triggering == UACPI_TRIGGERING_LEVEL;
        l->low = r->irq.polarity == UACPI_POLARITY_ACTIVE_LOW;
    } else if (r->type == UACPI_RESOURCE_TYPE_EXTENDED_IRQ && r->extended_irq.num_irqs) {
        l->gsi = r->extended_irq.irqs[0];
        l->level = r->extended_irq.triggering == UACPI_TRIGGERING_LEVEL;
        l->low = r->extended_irq.polarity == UACPI_POLARITY_ACTIVE_LOW;
    } else {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    l->found = true;
    return UACPI_ITERATION_DECISION_BREAK;
}

static uacpi_iteration_decision found_root_bridge(void *user, uacpi_namespace_node *node, uacpi_u32 depth)
{
    (void)depth;
    uacpi_pci_routing_table *t = NULL;
    if (*(bool *)user || uacpi_get_pci_routing_table(node, &t) != UACPI_STATUS_OK) return UACPI_ITERATION_DECISION_CONTINUE;
    *(bool *)user = true;                         /* the first root bridge: bus 0 */
    int links = 0;
    for (uacpi_size i = 0; i < t->num_entries && g_nprt < MAX_ROUTES; i++) {
        uacpi_pci_routing_table_entry *e = &t->entries[i];
        PciRoute r = { (UINT8)(e->address >> 16), e->pin, true, true, e->index };
        if (e->source) {                          /* a link device: its current IRQ */
            LinkIrq l = { 0 };
            uacpi_for_each_device_resource(e->source, "_CRS", link_resource, &l);
            if (!l.found) continue;
            r.gsi = l.gsi; r.level = l.level; r.low = l.low;
            links++;
        }
        g_prt[g_nprt++] = r;
    }
    const uacpi_char *path = uacpi_namespace_node_generate_absolute_path(node);
    kprintf("[ACPI] PCI interrupt routing: %d entries from %s._PRT (%d through link devices)\n",
            g_nprt, path ? path : "?", links);
    uacpi_free_absolute_path(path);
    uacpi_free_pci_routing_table(t);
    return UACPI_ITERATION_DECISION_CONTINUE;
}

bool AmlPciIrq(UINT8 dev, UINT8 pin, UINT32 *gsi, bool *level, bool *low)
{
    for (int i = 0; i < g_nprt; i++) {
        if (g_prt[i].dev != dev || g_prt[i].pin != pin) continue;
        if (gsi) *gsi = g_prt[i].gsi;
        if (level) *level = g_prt[i].level;
        if (low) *low = g_prt[i].low;
        return true;
    }
    return false;
}

/* Each function on bus 0 that uses an interrupt pin: its GSI goes in the
 * Interrupt Line register, as a PC BIOS would leave it */
static void route_pci(void)
{
    int routed = 0, missing = 0;
    for (UINT8 dev = 0; dev < 32; dev++)
        for (UINT8 fn = 0; fn < 8; fn++) {
            UINT32 id = PciRead32(0, dev, fn, 0);
            if ((id & 0xFFFF) == 0xFFFF) { if (!fn) break; continue; }
            UINT32 r3c = PciRead32(0, dev, fn, 0x3C);
            UINT8 pin = (UINT8)(r3c >> 8);
            UINT32 gsi;
            if (pin >= 1 && pin <= 4) {
                if (AmlPciIrq(dev, pin - 1, &gsi, NULL, NULL) && gsi < 255) {
                    PciWrite32(0, dev, fn, 0x3C, (r3c & ~0xFFu) | gsi);
                    routed++;
                } else {
                    missing++;
                }
            }
            if (!fn && !(PciRead32(0, dev, 0, 0x0C) & 0x00800000)) break;   /* single-function */
        }
    if (g_nprt) kprintf("[ACPI] Routed the interrupt pins of %d PCI function(s) on bus 0%s\n", routed,
                        missing ? " (some have no _PRT entry)" : "");
}

/* -----------------------------------------------------------------------
 * Wake devices (_PRW): the lid, power buttons and USB controllers wake the
 * machine from S3
 * ----------------------------------------------------------------------- */
#define MAX_WAKE 16

typedef struct { uacpi_namespace_node *node; UINT16 gpe; char what[24]; } WakeDev;
static WakeDev g_wake[MAX_WAKE];
static int     g_nwake;

static bool wake_gpe(uacpi_namespace_node *node, UINT16 *gpe)
{
    uacpi_object *o = NULL;
    uacpi_object_array a;
    uacpi_u64 v;
    bool ok = false;
    if (uacpi_eval_simple_package(node, "_PRW", &o) != UACPI_STATUS_OK) return false;
    /* (the GPE may also be a package naming a GPE block device; only \_GPE ones here) */
    if (uacpi_object_get_package(o, &a) == UACPI_STATUS_OK && a.count >= 2 &&
        uacpi_object_get_integer(a.objects[0], &v) == UACPI_STATUS_OK) {
        *gpe = (UINT16)v;
        ok = true;
    }
    uacpi_object_unref(o);
    return ok;
}

static void add_wake(uacpi_namespace_node *node, const char *what)
{
    UINT16 gpe;
    if (g_nwake == MAX_WAKE || !wake_gpe(node, &gpe)) return;
    if (uacpi_setup_gpe_for_wake(NULL, gpe, node) != UACPI_STATUS_OK) return;
    WakeDev *w = &g_wake[g_nwake++];
    w->node = node;
    w->gpe = gpe;
    strncpy(w->what, what, sizeof(w->what) - 1);
    const uacpi_char *path = uacpi_namespace_node_generate_absolute_path(node);
    kprintf("[ACPI] Wake device %s (%s): GPE 0x%x\n", path ? path : "?", what, gpe);
    uacpi_free_absolute_path(path);
}

static uacpi_iteration_decision found_wake_button(void *user, uacpi_namespace_node *node, uacpi_u32 depth)
{
    (void)depth;
    add_wake(node, user);
    return UACPI_ITERATION_DECISION_CONTINUE;
}

/* PCI functions on bus 0 described in the namespace (_ADR): the USB host
 * controllers among them */
static uacpi_iteration_decision found_pci_child(void *user, uacpi_namespace_node *node, uacpi_u32 depth)
{
    (void)user; (void)depth;
    uacpi_u64 adr;
    if (uacpi_eval_adr(node, &adr) != UACPI_STATUS_OK) return UACPI_ITERATION_DECISION_CONTINUE;
    UINT8 dev = (UINT8)(adr >> 16), fn = (UINT8)adr;
    if (dev > 31 || fn > 7) return UACPI_ITERATION_DECISION_CONTINUE;
    UINT32 cls = PciRead32(0, dev, fn, 0x08) >> 8;
    if ((PciRead32(0, dev, fn, 0) & 0xFFFF) != 0xFFFF && (cls >> 8) == 0x0C03)
        add_wake(node, "USB controller");
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_iteration_decision found_bridge_for_wake(void *user, uacpi_namespace_node *node, uacpi_u32 depth)
{
    (void)user; (void)depth;
    uacpi_namespace_for_each_child(node, found_pci_child, NULL, UACPI_OBJECT_DEVICE_BIT, 1, NULL);
    return UACPI_ITERATION_DECISION_BREAK;
}

/* _DSW (ACPI 3+) or _PSW: tell the device whether it may wake the machine */
static void device_wake(uacpi_namespace_node *node, bool on)
{
    uacpi_object *args[3] = { uacpi_object_create_integer(on), uacpi_object_create_integer(3),
                              uacpi_object_create_integer(3) };
    uacpi_object_array a = { args, 3 };
    if (uacpi_execute(node, "_DSW", &a) == UACPI_STATUS_NOT_FOUND) {
        a.count = 1;
        uacpi_execute(node, "_PSW", &a);
    }
    for (int i = 0; i < 3; i++) uacpi_object_unref(args[i]);
}

static void arm_wake(void)
{
    for (int i = 0; i < g_nwake; i++) {
        device_wake(g_wake[i].node, true);
        uacpi_enable_gpe_for_wake(NULL, g_wake[i].gpe);
    }
}

/* Which wake device's GPE fired; the fixed power button or the RTC alarm */
static void report_wake(void)
{
    for (int i = 0; i < g_nwake; i++) {
        uacpi_event_info info = 0;
        if (uacpi_gpe_info(NULL, g_wake[i].gpe, &info) == UACPI_STATUS_OK && (info & UACPI_EVENT_INFO_HW_STATUS)) {
            const uacpi_char *path = uacpi_namespace_node_generate_absolute_path(g_wake[i].node);
            kprintf("[ACPI] Woken by %s (%s)\n", path ? path : "?", g_wake[i].what);
            uacpi_free_absolute_path(path);
            return;
        }
    }
    UINT16 sts = AcpiWakeStatus();       /* (read before AcpiResume cleared it) */
    if (sts & (1u << 8))
        kprintf("[ACPI] Woken by the power button (or a device the platform reports as it)\n");
    else if (sts & (1u << 10))
        kprintf("[ACPI] Woken by the RTC alarm\n");
    else
        kprintf("[ACPI] Woken (no wake status set)\n");
}

/* -----------------------------------------------------------------------
 * Sleep
 * ----------------------------------------------------------------------- */
static bool g_slept;

void AmlPrepareSleep(void)
{
    g_slept = g_ready;
    if (!g_slept) return;
    uacpi_status st = uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S3);
    if (st != UACPI_STATUS_OK)
        kprintf("[ACPI] \\_PTS failed: %s\n", uacpi_status_to_string(st));
    arm_wake();                          /* _DSW / _PSW, and their GPEs */
    uacpi_disable_all_gpes();
    uacpi_enable_all_wake_gpes();
}

void AmlWake(void)
{
    if (!g_slept) return;
    g_slept = false;
    report_wake();                       /* (before \_WAK clears the status bits) */
    for (int i = 0; i < g_nwake; i++) device_wake(g_wake[i].node, false);
    uacpi_status st = uacpi_wake_from_sleep_state(UACPI_SLEEP_STATE_S3);
    if (st != UACPI_STATUS_OK)
        kprintf("[ACPI] \\_WAK failed: %s\n", uacpi_status_to_string(st));
    __atomic_store_n(&g_refresh, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_lid_read, 1, __ATOMIC_RELEASE);       /* the lid may have opened */
    __atomic_store_n(&g_zone_read, 1, __ATOMIC_RELEASE);
    kick();
}

/* -----------------------------------------------------------------------
 * The acpi thread
 * ----------------------------------------------------------------------- */
static bool load(void)
{
    uacpi_status st = uacpi_initialize(0);
    if (st == UACPI_STATUS_OK) st = uacpi_namespace_load();
    if (st == UACPI_STATUS_OK && IoApicPresent()) st = uacpi_set_interrupt_model(UACPI_INTERRUPT_MODEL_IOAPIC);
    if (st == UACPI_STATUS_OK) st = uacpi_namespace_initialize();
    if (st != UACPI_STATUS_OK) {
        kprintf("[ACPI] The AML interpreter didn't start: %s\n", uacpi_status_to_string(st));
        return false;
    }
    if (uacpi_install_fixed_event_handler(UACPI_FIXED_EVENT_POWER_BUTTON, fixed_power_button, NULL)
        == UACPI_STATUS_OK)
        kprintf("[ACPI] Power button (fixed)\n");
    uacpi_find_devices("PNP0C0C", found_button, NULL);
    uacpi_find_devices("PNP0C0A", found_battery, NULL);
    uacpi_find_devices("ACPI0003", found_adapter, NULL);
    uacpi_find_devices("PNP0C0D", found_lid, NULL);
    uacpi_namespace_for_each_child(uacpi_namespace_root(), found_zone, NULL,
                                   UACPI_OBJECT_THERMAL_ZONE_BIT, UACPI_MAX_DEPTH_ANY, NULL);
    if (IoApicPresent()) {               /* (in PIC mode _PRT names ISA IRQs, which nothing routes) */
        static const uacpi_char *roots[] = { "PNP0A08", "PNP0A03", NULL };
        bool found = false;
        uacpi_find_devices_at(uacpi_namespace_root(), roots, found_root_bridge, &found);
        route_pci();
    }
    /* wake devices, before the GPEs are enabled (wake GPEs stay off while awake) */
    uacpi_find_devices("PNP0C0D", found_wake_button, "lid");
    uacpi_find_devices("PNP0C0C", found_wake_button, "power button");
    {
        static const uacpi_char *roots[] = { "PNP0A08", "PNP0A03", NULL };
        uacpi_find_devices_at(uacpi_namespace_root(), roots, found_bridge_for_wake, NULL);
    }
    uacpi_finalize_gpe_initialization();
    return true;
}

static void acpi_thread(void *arg)
{
    (void)arg;
    bkl_release();                       /* AML runs under uACPI's own locks */
    if (!load()) sched_exit_current();
    g_ready = true;
    UINT64 next = 0, next_sci = 0;
    for (;;) {
        __atomic_store_n(&g_kick, 0, __ATOMIC_RELEASE);
        if (g_sci && sched_ticks() >= next_sci) {     /* a missed edge, or no interrupt at all */
            g_sci(g_sci_ctx);
            next_sci = sched_ticks() + (g_sci_irq ? 100 : 10);
        }
        run_work();
        if (__atomic_exchange_n(&g_lid_read, 0, __ATOMIC_ACQ_REL)) read_lid();
        poll_zones(__atomic_exchange_n(&g_zone_read, 0, __ATOMIC_ACQ_REL) != 0);
        if (__atomic_exchange_n(&g_refresh, 0, __ATOMIC_ACQ_REL) || sched_ticks() >= next) {
            refresh();
            next = sched_ticks() + 500;
        }
        UINT64 wake = next < next_sci ? next : next_sci;
        for (int i = 0; i < g_nzone; i++) if (g_zone[i].next < wake) wake = g_zone[i].next;
        sched_sleep_until(&g_kick, wake);
    }
}

void AmlInitialize(void)
{
    if (!AcpiRsdpAddress()) return;
    g_thread = sched_create_thread("acpi", acpi_thread, NULL, 8);
}

bool AmlReady(void) { return g_ready; }
