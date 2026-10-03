/*
 * ec.c — the ACPI embedded controller (ACPI 6.5, chapter 12)
 *
 * A laptop's firmware keeps the lid switch, the battery and the AC adapter
 * behind its embedded controller: their AML reads fields of an
 * EmbeddedControl operation region, and the controller raises an event
 * (a GPE) when one of them changes.  uACPI has no embedded controller of
 * its own, so this file is the address space handler it calls for those
 * fields, and the event handler.
 *
 * The controller: two I/O ports, data and command/status, from the ECDT
 * (the table firmware provides so the controller works before the
 * namespace loads) or from the PNP0C09 device's _CRS.  A byte is read
 * with RD_EC (0x80) and its address, written with WR_EC (0x81), address
 * and value; each byte waits for the input buffer to empty (IBF clear)
 * and an answer for the output buffer to fill (OBF set).  An event sets
 * SCI_EVT in the status and raises the controller's GPE (_GPE, or the
 * ECDT's); QR_EC (0x84) then returns its number, and the firmware's
 * method \...EC._Qxx (xx the number in hex) handles it, usually with a
 * Notify to the lid or the battery.  _GLK = 1 asks for the ACPI global
 * lock around each access.
 *
 * The self-tests have no embedded controller (QEMU emulates none): a
 * PNP0C09 device whose _HID is NOVA0EC1 (tests/acpi/laptop.asl) is
 * served by a model of one in this file instead of I/O ports.
 */

#include "ec.h"
#include "../ke/printf.h"
#include "../ke/scheduler.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/apic.h"
#include "../lib/string.h"

#include <uacpi/uacpi.h>
#include <uacpi/acpi.h>
#include <uacpi/event.h>
#include <uacpi/opregion.h>
#include <uacpi/tables.h>
#include <uacpi/resources.h>
#include <uacpi/utilities.h>
#include <uacpi/kernel_api.h>
#include <uacpi/internal/context.h>     /* (the global lock's owner, below) */
#include <uacpi/internal/mutex.h>

#define EC_OBF      0x01        /* output buffer full: a byte to read */
#define EC_IBF      0x02        /* input buffer full: the controller hasn't taken the last byte */
#define EC_SCI_EVT  0x20        /* an event is waiting for QR_EC */

#define RD_EC       0x80
#define WR_EC       0x81
#define QR_EC       0x84

typedef struct {
    uacpi_namespace_node *node;
    UINT16       data, cmd;            /* I/O ports */
    UINT16       gpe;
    bool         have_gpe, glk, model;
    uacpi_handle lock;
    UINT32       timeouts;
    UINT8        seen[32];             /* event numbers logged once */
} Ec;

static Ec              g_ec;
static bool            g_present;
static volatile UINT32 g_query_queued;

/* -----------------------------------------------------------------------
 * The self-tests' controller: 256 bytes of registers, and an event raised
 * when AML writes its number to the last one (the test's "raise event"
 * register; a real controller raises them itself)
 * ----------------------------------------------------------------------- */
static struct {
    UINT8 ram[256];
    UINT8 status, out, cmd, addr;
    int   stage;                       /* bytes of the command taken so far */
    UINT8 events[8];
    int   nevents;
} g_model;

static void model_output(UINT8 v) { g_model.out = v; g_model.status |= EC_OBF; }

static void model_command(UINT8 c)
{
    g_model.cmd = c;
    g_model.stage = 0;
    if (c == QR_EC) {
        UINT8 q = 0;
        if (g_model.nevents) {
            q = g_model.events[0];
            memmove(g_model.events, g_model.events + 1, --g_model.nevents);
        }
        if (!g_model.nevents) g_model.status &= ~EC_SCI_EVT;
        model_output(q);
    }
}

static void model_data(UINT8 v)
{
    if (g_model.cmd == RD_EC && g_model.stage == 0) {
        model_output(g_model.ram[v]);
        g_model.stage = 1;
    } else if (g_model.cmd == WR_EC && g_model.stage == 0) {
        g_model.addr = v;
        g_model.stage = 1;
    } else if (g_model.cmd == WR_EC && g_model.stage == 1) {
        g_model.ram[g_model.addr] = v;
        g_model.stage = 2;
        if (g_model.addr == 0xFF && v && g_model.nevents < (int)sizeof(g_model.events)) {
            g_model.events[g_model.nevents++] = v;
            g_model.status |= EC_SCI_EVT;
        }
    }
}

/* -----------------------------------------------------------------------
 * The ports
 * ----------------------------------------------------------------------- */
static UINT8 rd_status(void) { return g_ec.model ? g_model.status : inb(g_ec.cmd); }

static UINT8 rd_data(void)
{
    if (!g_ec.model) return inb(g_ec.data);
    g_model.status &= ~EC_OBF;
    return g_model.out;
}

static void wr_cmd(UINT8 v)  { if (g_ec.model) model_command(v); else outb(g_ec.cmd, v); }
static void wr_data(UINT8 v) { if (g_ec.model) model_data(v); else outb(g_ec.data, v); }

/* Wait for @bit of the status to become @set: up to half a second (the
 * specification allows the controller 1 ms per byte; some take longer) */
static bool wait_status(UINT8 bit, bool set)
{
    for (int i = 0; i < 50000; i++) {
        if (((rd_status() & bit) != 0) == set) return true;
        udelay(10);
    }
    if (g_ec.timeouts++ < 8)
        kprintf("[EC] Timed out waiting for %s (status 0x%02x)\n", bit == EC_IBF ? "the input buffer" : "an answer",
                rd_status());
    return false;
}

static bool send_cmd(UINT8 c)
{
    if (rd_status() & EC_OBF) (void)rd_data();       /* a stale answer */
    if (!wait_status(EC_IBF, false)) return false;
    wr_cmd(c);
    return wait_status(EC_IBF, false);
}

static bool ec_read(UINT8 addr, UINT8 *v)
{
    if (!send_cmd(RD_EC)) return false;
    wr_data(addr);
    if (!wait_status(EC_OBF, true)) return false;
    *v = rd_data();
    return true;
}

static bool ec_write(UINT8 addr, UINT8 v)
{
    if (!send_cmd(WR_EC)) return false;
    wr_data(addr);
    if (!wait_status(EC_IBF, false)) return false;
    wr_data(v);
    return wait_status(EC_IBF, false);
}

static bool ec_query(UINT8 *q)
{
    if (!send_cmd(QR_EC) || !wait_status(EC_OBF, true)) return false;
    *q = rd_data();
    return true;
}

/* The controller, and the global lock when _GLK asks for it, unless this
 * thread's AML already holds that (a field declared with Lock: uACPI's
 * global lock mutex doesn't nest).  Returns whether it took the global
 * lock, with its sequence number in *seq. */
static bool lock(uacpi_u32 *seq)
{
    uacpi_kernel_acquire_mutex(g_ec.lock, 0xFFFF);
    return g_ec.glk && !uacpi_this_thread_owns_aml_mutex(g_uacpi_rt_ctx.global_lock_mutex) &&
           uacpi_acquire_global_lock(0xFFFF, seq) == UACPI_STATUS_OK;
}

static void unlock(bool glk, uacpi_u32 seq)
{
    if (glk) uacpi_release_global_lock(seq);
    uacpi_kernel_release_mutex(g_ec.lock);
}

/* -----------------------------------------------------------------------
 * The EmbeddedControl address space
 * ----------------------------------------------------------------------- */
static uacpi_status ec_region(uacpi_region_op op, uacpi_handle opdata)
{
    if (op == UACPI_REGION_OP_ATTACH || op == UACPI_REGION_OP_DETACH) return UACPI_STATUS_OK;
    if (op != UACPI_REGION_OP_READ && op != UACPI_REGION_OP_WRITE) return UACPI_STATUS_INVALID_ARGUMENT;
    uacpi_region_rw_data *rw = opdata;
    if (rw->offset + rw->byte_width > 256) return UACPI_STATUS_AML_OUT_OF_BOUNDS_INDEX;
    uacpi_u32 seq = 0;
    bool glk = lock(&seq), ok = true;
    if (op == UACPI_REGION_OP_READ) {
        rw->value = 0;
        for (int i = 0; ok && i < rw->byte_width; i++) {
            UINT8 b = 0;
            ok = ec_read((UINT8)(rw->offset + i), &b);
            rw->value |= (uacpi_u64)b << (8 * i);
        }
    } else {
        for (int i = 0; ok && i < rw->byte_width; i++)
            ok = ec_write((UINT8)(rw->offset + i), (UINT8)(rw->value >> (8 * i)));
    }
    unlock(glk, seq);
    return ok ? UACPI_STATUS_OK : UACPI_STATUS_HARDWARE_TIMEOUT;
}

/* -----------------------------------------------------------------------
 * Events
 * ----------------------------------------------------------------------- */
static void run_queries(uacpi_handle ctx)
{
    (void)ctx;
    __atomic_store_n(&g_query_queued, 0, __ATOMIC_RELEASE);
    for (int n = 0; n < 32; n++) {
        uacpi_u32 seq = 0;
        bool glk = lock(&seq);
        UINT8 q = 0;
        bool ok = (rd_status() & EC_SCI_EVT) && ec_query(&q);
        unlock(glk, seq);
        if (!ok || !q) return;
        char name[5] = { '_', 'Q', "0123456789ABCDEF"[q >> 4], "0123456789ABCDEF"[q & 15], 0 };
        uacpi_status st = uacpi_execute(g_ec.node, name, NULL);
        if (!(g_ec.seen[q >> 3] & (1u << (q & 7)))) {
            g_ec.seen[q >> 3] |= (UINT8)(1u << (q & 7));
            kprintf("[EC] Event 0x%02x: %s\n", q, st == UACPI_STATUS_OK ? name :
                    st == UACPI_STATUS_NOT_FOUND ? "no method for it" : uacpi_status_to_string(st));
        }
    }
}

static void queue_queries(void)
{
    if (!__atomic_exchange_n(&g_query_queued, 1, __ATOMIC_ACQ_REL) &&
        uacpi_kernel_schedule_work(UACPI_WORK_GPE_EXECUTION, run_queries, NULL) != UACPI_STATUS_OK)
        __atomic_store_n(&g_query_queued, 0, __ATOMIC_RELEASE);
}

static uacpi_interrupt_ret ec_gpe(uacpi_handle ctx, uacpi_namespace_node *dev, uacpi_u16 idx)
{
    (void)ctx; (void)dev; (void)idx;
    queue_queries();
    return UACPI_INTERRUPT_HANDLED | UACPI_GPE_REENABLE;
}

void EcPoll(void)
{
    if (g_present && (rd_status() & EC_SCI_EVT)) run_queries(NULL);
}

/* -----------------------------------------------------------------------
 * Finding it
 * ----------------------------------------------------------------------- */
typedef struct { UINT16 port[2]; int n; } Ports;

static uacpi_iteration_decision found_port(void *user, uacpi_resource *r)
{
    Ports *p = user;
    if (p->n < 2 && r->type == UACPI_RESOURCE_TYPE_IO) p->port[p->n++] = r->io.minimum;
    else if (p->n < 2 && r->type == UACPI_RESOURCE_TYPE_FIXED_IO) p->port[p->n++] = r->fixed_io.address;
    return p->n < 2 ? UACPI_ITERATION_DECISION_CONTINUE : UACPI_ITERATION_DECISION_BREAK;
}

static uacpi_iteration_decision found_device(void *user, uacpi_namespace_node *node, uacpi_u32 depth)
{
    (void)depth;
    Ec *ec = user;
    Ports p = { { 0, 0 }, 0 };
    uacpi_for_each_device_resource(node, "_CRS", found_port, &p);
    if (p.n < 2) return UACPI_ITERATION_DECISION_CONTINUE;
    ec->node = node;
    ec->data = p.port[0];                          /* (data first, then command/status) */
    ec->cmd = p.port[1];
    uacpi_u64 gpe;
    if (uacpi_eval_simple_integer(node, "_GPE", &gpe) == UACPI_STATUS_OK) {
        ec->gpe = (UINT16)gpe;
        ec->have_gpe = true;
    }
    return UACPI_ITERATION_DECISION_BREAK;
}

/* The ECDT's controller (its namespace path names the device) */
static bool from_ecdt(Ec *ec)
{
    uacpi_table t;
    if (uacpi_table_find_by_signature(ACPI_ECDT_SIGNATURE, &t) != UACPI_STATUS_OK) return false;
    struct acpi_ecdt *e = t.ptr;
    bool ok = e->hdr.length > sizeof(*e) && e->ec_control.address && e->ec_data.address &&
              e->ec_control.address_space_id == UACPI_ADDRESS_SPACE_SYSTEM_IO;
    if (ok) {
        ec->cmd = (UINT16)e->ec_control.address;
        ec->data = (UINT16)e->ec_data.address;
        ec->gpe = e->gpe_bit;
        ec->have_gpe = true;
        if (uacpi_namespace_node_find(NULL, e->ec_id, &ec->node) != UACPI_STATUS_OK) ec->node = NULL;
    }
    uacpi_table_unref(&t);
    return ok && ec->node;
}

void EcProbe(void)
{
    Ec ec;
    memset(&ec, 0, sizeof(ec));
    bool ecdt = from_ecdt(&ec);
    if (!ecdt) uacpi_find_devices("PNP0C09", found_device, &ec);
    if (!ec.node) return;

    uacpi_id_string *hid = NULL;
    if (uacpi_eval_hid(ec.node, &hid) == UACPI_STATUS_OK && hid && !strcmp(hid->value, "NOVA0EC1"))
        ec.model = true;
    uacpi_free_id_string(hid);
    uacpi_u64 glk = 0;
    ec.glk = uacpi_eval_simple_integer(ec.node, "_GLK", &glk) == UACPI_STATUS_OK && glk;
    ec.lock = uacpi_kernel_create_mutex();
    g_ec = ec;
    g_present = true;

    const uacpi_char *path = uacpi_namespace_node_generate_absolute_path(ec.node);
    char line[160];
    int k = ec.model ? ksnprintf(line, sizeof(line), "%s: the self-tests' embedded controller (a model, no I/O ports)",
                                 path ? path : "?")
                     : ksnprintf(line, sizeof(line), "%s: ports 0x%x/0x%x (from the %s)", path ? path : "?",
                                 ec.data, ec.cmd, ecdt ? "ECDT" : "_CRS");
    if (ec.have_gpe) k += ksnprintf(line + k, sizeof(line) - k, ", GPE 0x%x", ec.gpe);
    if (ec.glk) ksnprintf(line + k, sizeof(line) - k, ", global lock");
    kprintf("[EC] %s\n", line);
    uacpi_free_absolute_path(path);

    /* (runs the controller's _REG, which lets its AML use the fields) */
    uacpi_status st = uacpi_install_address_space_handler(ec.node, UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER,
                                                          ec_region, NULL);
    if (st != UACPI_STATUS_OK)
        kprintf("[EC] Could not install the address space handler: %s\n", uacpi_status_to_string(st));
    if (ec.have_gpe) {
        st = uacpi_install_gpe_handler(NULL, ec.gpe, UACPI_GPE_TRIGGERING_EDGE, ec_gpe, NULL);
        if (st == UACPI_STATUS_OK) st = uacpi_enable_gpe(NULL, ec.gpe);
        if (st != UACPI_STATUS_OK)
            kprintf("[EC] GPE 0x%x: %s (events are polled)\n", ec.gpe, uacpi_status_to_string(st));
    }
}

bool EcPresent(void) { return g_present; }
