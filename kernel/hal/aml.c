/*
 * aml.c — the ACPI namespace: uACPI's host functions, the "acpi" thread,
 * batteries, AC adapters and power buttons
 *
 * uACPI (third_party/uacpi, MIT) interprets the DSDT and SSDTs.  This file
 * gives it memory, I/O ports, PCI configuration space, time, locks and a
 * work queue.  There is no SCI interrupt: the "acpi" thread calls uACPI's
 * SCI handler every 100 ms, which checks the event status registers, and
 * then runs the GPE methods and Notify handlers that queued work.
 *
 * Battery readings are cached by that thread (every 5 s, and when the
 * firmware notifies a change), so system calls never run AML.
 */

#include "aml.h"
#include "acpi.h"
#include "pci.h"
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

/* The SCI: polled by the acpi thread */
static uacpi_interrupt_handler g_sci;
static uacpi_handle            g_sci_ctx;

uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 irq, uacpi_interrupt_handler h,
                                                    uacpi_handle ctx, uacpi_handle *out)
{
    (void)irq;
    g_sci_ctx = ctx;
    g_sci = h;
    *out = (uacpi_handle)&g_sci;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler h, uacpi_handle irq)
{
    (void)h; (void)irq;
    g_sci = NULL;
    return UACPI_STATUS_OK;
}

/* Work (GPE methods, Notify handlers): queued from the SCI handler, run
 * by the acpi thread */
#define WORK_MAX 64
typedef struct { uacpi_work_handler fn; uacpi_handle ctx; } Work;
static Work            g_work[WORK_MAX];
static UINT32          g_work_head, g_work_tail;
static volatile UINT32 g_work_busy;      /* queued or running */
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
    uacpi_disable_all_gpes();
    uacpi_enable_all_wake_gpes();
}

void AmlWake(void)
{
    if (!g_slept) return;
    g_slept = false;
    uacpi_status st = uacpi_wake_from_sleep_state(UACPI_SLEEP_STATE_S3);
    if (st != UACPI_STATUS_OK)
        kprintf("[ACPI] \\_WAK failed: %s\n", uacpi_status_to_string(st));
    __atomic_store_n(&g_refresh, 1, __ATOMIC_RELEASE);
}

/* -----------------------------------------------------------------------
 * The acpi thread
 * ----------------------------------------------------------------------- */
static bool load(void)
{
    uacpi_status st = uacpi_initialize(0);
    if (st == UACPI_STATUS_OK) st = uacpi_namespace_load();
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
    uacpi_finalize_gpe_initialization();
    return true;
}

static void acpi_thread(void *arg)
{
    (void)arg;
    bkl_release();                       /* AML runs under uACPI's own locks */
    if (!load()) sched_exit_current();
    g_ready = true;
    UINT64 next = 0;
    for (;;) {
        if (g_sci) g_sci(g_sci_ctx);
        run_work();
        if (__atomic_exchange_n(&g_refresh, 0, __ATOMIC_ACQ_REL) || sched_ticks() >= next) {
            refresh();
            next = sched_ticks() + 500;
        }
        sched_sleep_until(NULL, sched_ticks() + 10);
    }
}

void AmlInitialize(void)
{
    if (!AcpiRsdpAddress()) return;
    g_thread = sched_create_thread("acpi", acpi_thread, NULL, 8);
}

bool AmlReady(void) { return g_ready; }
