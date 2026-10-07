/*
 * vmbus.c — Hyper-V's VMBus: hypercalls, the SynIC, channel offers and rings
 *
 * The steps (Hyper-V's Top Level Functional Specification for the
 * hypervisor part; the VMBus messages and their layouts are the ones
 * Linux's hv_vmbus and FreeBSD's vmbus drivers use):
 *
 *   1. CPUID 0x40000000 says "Microsoft Hv"; 0x40000003 lists the SynIC,
 *      the hypercall MSRs and the PostMessage / SignalEvent hypercalls.
 *   2. A guest OS ID, then the hypercall page: the hypervisor fills a page
 *      the guest names with the code that makes a hypercall (vmcall or
 *      vmmcall).  It lives in the physmap, which is executable.
 *   3. The SynIC on CPU 0: the message page (one 256-byte slot per
 *      synthetic interrupt source, SINT), the event flags page, SINT 2 (the
 *      one VMBus uses) unmasked on its own vector.
 *   4. INITIATE_CONTACT with the newest VMBus version first, until the host
 *      takes one; then REQUESTOFFERS: one OFFERCHANNEL message per device,
 *      then ALLOFFERS_DELIVERED.
 *   5. For each device a NovaOS driver takes: its rings (one block of
 *      pages, outbound then inbound) described to the host as a GPADL,
 *      then OPENCHANNEL.  The host answers each with a message.
 *
 * Everything that waits for a message runs once, in VmbusInit on CPU 0
 * before interrupts are on, polling the message slot (the SynIC is per
 * CPU).  After that the channels need no messages: VmbusPoll reads their
 * inbound rings from the device poll thread (the inbound rings say "no
 * signal": interrupt_mask), and VmbusSend writes the outbound one and
 * signals the host with the SignalEvent hypercall, which any CPU may make.
 */

#include "vmbus.h"
#include "vmbus_ring.h"
#include "hv_input.h"
#include "../hal/ioapic.h"
#include "../arch/x86_64/idt.h"
#include "../arch/x86_64/apic.h"
#include "../arch/x86_64/cpu.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../ke/printf.h"

#define HV_MSR_GUEST_OS_ID   0x40000000
#define HV_MSR_HYPERCALL     0x40000001
#define HV_MSR_VP_INDEX      0x40000002
#define HV_MSR_SCONTROL      0x40000080
#define HV_MSR_SIEFP         0x40000082
#define HV_MSR_SIMP          0x40000083
#define HV_MSR_EOM           0x40000084
#define HV_MSR_SINT0         0x40000090

#define HV_FEAT_SYNIC        (1u << 2)    /* CPUID 0x40000003 EAX */
#define HV_FEAT_HYPERCALL    (1u << 5)
#define HV_FEAT_VP_INDEX     (1u << 6)
#define HV_PRIV_POST_MESSAGE (1u << 4)    /* ... EBX */
#define HV_PRIV_SIGNAL_EVENT (1u << 5)

#define HVCALL_POST_MESSAGE  0x005C
#define HVCALL_SIGNAL_EVENT  0x005D
#define HV_HYPERCALL_FAST    (1ull << 16)
#define HV_STATUS_INSUFFICIENT_BUFFERS 0x13

/* An open-source OS (bit 63), NovaOS's own OS type, version 1 */
#define NOVAOS_GUEST_ID      ((1ull << 63) | (0x7Full << 56) | (1ull << 16))

#define VMBUS_SINT           2
#define VMBUS_CONN_MESSAGE   1            /* where messages go before VMBus 5.0 */
#define VMBUS_CONN_MESSAGE_5 4            /* ... from 5.0 (the host may name another) */

/* VMBus channel messages */
#define CHMSG_OFFERCHANNEL       1
#define CHMSG_REQUESTOFFERS      3
#define CHMSG_ALLOFFERS_DELIVERED 4
#define CHMSG_OPENCHANNEL        5
#define CHMSG_OPENCHANNEL_RESULT 6
#define CHMSG_GPADL_HEADER       8
#define CHMSG_GPADL_CREATED      10
#define CHMSG_INITIATE_CONTACT   14
#define CHMSG_VERSION_RESPONSE   15

#define RING_PAGES   10                   /* each way, as Linux's input drivers */
#define MAX_OFFERS   32
#define MAX_CHANNELS 4
#define PKT_MAX      2048

/* A message slot of the SynIC message page */
typedef struct {
    volatile UINT32 type;                 /* 0: empty */
    UINT8           size;
    volatile UINT8  flags;                /* bit 0: another one waits (write EOM) */
    UINT8           reserved[2];
    UINT64          sender;
    UINT8           payload[240];
} HvMessage;

typedef struct __attribute__((packed)) {
    UINT32 msgtype, padding;
} ChHeader;

typedef struct __attribute__((packed)) {
    ChHeader h;
    UINT32   version;
    UINT32   target_vcpu;
    union {
        UINT64 interrupt_page;            /* before 5.0 */
        struct { UINT8 msg_sint, msg_vtl, reserved[2]; UINT32 features; };
    };
    UINT64   monitor_page1, monitor_page2;
} ChInitiateContact;

typedef struct __attribute__((packed)) {
    ChHeader h;
    UINT8    supported, state;
    UINT16   padding;
    UINT32   msg_conn_id;
} ChVersionResponse;

typedef struct __attribute__((packed)) {
    ChHeader h;
    UINT8    if_type[16], if_instance[16];
    UINT64   reserved1, reserved2;
    UINT16   chn_flags, mmio_megabytes;
    UINT8    user_defined[120];
    UINT16   sub_channel_index, reserved3;
    UINT32   child_relid;
    UINT8    monitorid, monitor_allocated;
    UINT16   is_dedicated_interrupt;
    UINT32   connection_id;
} ChOffer;

typedef struct __attribute__((packed)) {
    ChHeader h;
    UINT32   child_relid, gpadl;
    UINT16   range_buflen, rangecount;
    UINT32   byte_count, byte_offset;
    UINT64   pfn[2 * RING_PAGES];
} ChGpadlHeader;

typedef struct __attribute__((packed)) {
    ChHeader h;
    UINT32   child_relid, gpadl, status;
} ChGpadlCreated;

typedef struct __attribute__((packed)) {
    ChHeader h;
    UINT32   child_relid, openid, gpadl, target_vp, downstream_offset;
    UINT8    user_data[120];
} ChOpenChannel;

typedef struct __attribute__((packed)) {
    ChHeader h;
    UINT32   child_relid, openid, status;
} ChOpenResult;

_Static_assert(sizeof(ChInitiateContact) == 40, "INITIATE_CONTACT");
_Static_assert(sizeof(ChOffer) == 196, "OFFERCHANNEL");
_Static_assert(sizeof(ChGpadlHeader) == 28 + 16 * RING_PAGES, "GPADL_HEADER");
_Static_assert(sizeof(ChGpadlHeader) <= 240, "a GPADL that needs GPADL_BODY messages");
_Static_assert(sizeof(ChOpenChannel) == 148, "OPENCHANNEL");

struct VmbusChannel {
    const VmbusDriver *drv;
    void   *ctx;
    VmRing  out, in;
    UINT32  relid, conn_id;
    bool    dedicated;
    UINT8   pkt[PKT_MAX];
};

typedef struct {
    UINT8  type[16];
    UINT32 relid, conn_id;
    bool   dedicated;
} Offer;

static const VmbusDriver *const g_drivers[] = { &HvKeyboardDriver, &HvMouseDriver };

static void         *g_hypercall_page;
static HvMessage    *g_simp;
static UINT8        *g_post;              /* the PostMessage input */
static UINT8        *g_int_page;          /* before 5.0: the interrupt bits (send half second) */
static UINT32        g_msg_conn;
static UINT32        g_vp;
static Offer         g_offers[MAX_OFFERS];
static int           g_noffers;
static VmbusChannel *g_ch[MAX_CHANNELS];
static int           g_nch;
static volatile bool g_ready;

static inline UINT64 phys(const void *va) { return (UINT64)(uintptr_t)va - PHYSMAP_BASE; }

static UINT64 hv_call(UINT64 control, UINT64 in, UINT64 out)
{
    UINT64 status;
    register UINT64 r8 __asm__("r8") = out;
    __asm__ volatile ("call *%[pg]"
                      : "=a"(status), "+c"(control), "+d"(in), "+r"(r8)
                      : [pg] "m"(g_hypercall_page)
                      : "cc", "memory", "r9", "r10", "r11");
    return status & 0xFFFF;
}

static UINT64 post_message(const void *msg, UINT32 len)
{
    UINT64 st = 0;
    for (int tries = 0; tries < 20; tries++) {
        UINT32 *p = (UINT32 *)g_post;
        p[0] = g_msg_conn;
        p[1] = 0;
        p[2] = 1;                         /* a VMBus channel message */
        p[3] = len;
        memcpy(g_post + 16, msg, len);
        st = hv_call(HVCALL_POST_MESSAGE, phys(g_post), 0);
        if (st != HV_STATUS_INSUFFICIENT_BUFFERS) break;
        udelay(1000);
    }
    return st;
}

/* The next VMBus message within @ms milliseconds: its type, payload in
 * @out (240 bytes); 0 when none came */
static UINT32 wait_message(UINT8 *out, UINT32 ms)
{
    HvMessage *slot = &g_simp[VMBUS_SINT];
    for (UINT32 t = 0; t <= ms * 10; t++) {
        if (slot->type) {
            memcpy(out, slot->payload, sizeof(slot->payload));
            slot->type = 0;
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            if (slot->flags & 1) wrmsr(HV_MSR_EOM, 0);    /* deliver the next one */
            return ((ChHeader *)out)->msgtype;
        }
        udelay(100);
    }
    return 0;
}

static void guid_text(const UINT8 *g, char *s, size_t cap)
{
    ksnprintf(s, cap, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
              g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6],
              g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

/* Devices worth a name in the log (NovaOS drives the keyboard and the mouse) */
static const struct { const char *guid, *name; } g_known[] = {
    { "f912ad6d-2b17-48ea-bd65-f927a61c7684", "keyboard" },
    { "cfa8b69e-5b4a-4cc0-b98b-8ba1a1f3f95a", "mouse" },
    { "da0a7802-e377-4aac-8e77-0558eb1073f8", "video (no driver: the firmware's frame buffer is used)" },
    { "ba6163d9-04a1-4d29-b605-72e2ffb1dc7f", "SCSI storage (no driver)" },
    { "f8615163-df3e-46c5-913f-f2d2f965ed0e", "network (no driver)" },
    { "0e0b6031-5213-4934-818b-38d90ced39db", "shutdown (no driver)" },
    { "9527e630-d0ae-497b-adce-e80ab0175caf", "time sync (no driver)" },
    { "57164f39-9115-4e78-ab55-382f3bd5422d", "heartbeat (no driver)" },
    { "a9a0f4e7-5a45-4d96-b827-8a841e8c03e6", "key-value exchange (no driver)" },
    { "525074dc-8985-46e2-8057-a307dc18a502", "dynamic memory (no driver)" },
};

static bool connect(void)
{
    static const UINT32 versions[] = { 0x50003, 0x50002, 0x50001, 0x50000, 0x40001, 0x40000, 0x30000 };
    UINT8 msg[240];
    for (UINT32 i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
        UINT32 v = versions[i];
        ChInitiateContact c;
        memset(&c, 0, sizeof(c));
        c.h.msgtype = CHMSG_INITIATE_CONTACT;
        c.version = v;
        c.target_vcpu = g_vp;
        if (v >= 0x50000) { c.msg_sint = VMBUS_SINT; g_msg_conn = VMBUS_CONN_MESSAGE_5; }
        else              { c.interrupt_page = phys(g_int_page); g_msg_conn = VMBUS_CONN_MESSAGE; }
        UINT8 *mon = kernel_alloc_pages(2);
        if (!mon) return false;
        memset(mon, 0, 2 * VMBUS_PAGE);
        c.monitor_page1 = phys(mon);
        c.monitor_page2 = phys(mon) + VMBUS_PAGE;
        UINT64 st = post_message(&c, sizeof(c));
        UINT32 got = st ? 0 : wait_message(msg, 1000);
        while (got && got != CHMSG_VERSION_RESPONSE) got = wait_message(msg, 1000);
        ChVersionResponse *r = (ChVersionResponse *)msg;
        if (got && r->supported) {
            if (v >= 0x50000 && r->msg_conn_id) g_msg_conn = r->msg_conn_id;
            kprintf("[VMBUS] Connected: VMBus %u.%u (CPU 0 is virtual processor %u)\n", v >> 16, v & 0xFFFF, g_vp);
            return true;
        }
        kernel_free_pages(mon, 2);
        if (st && v < 0x50000) {          /* (an older host has no connection 4: tried on) */
            kprintf("[VMBUS] Cannot post messages (hypercall status 0x%lx)\n", st);
            return false;
        }
    }
    kprintf("[VMBUS] The host took no VMBus version NovaOS speaks\n");
    return false;
}

static bool open_channel(const Offer *o, const VmbusDriver *drv)
{
    UINT8 msg[240];
    VmbusChannel *ch = kzalloc(sizeof(VmbusChannel));
    UINT8 *rings = kernel_alloc_pages(2 * RING_PAGES);
    if (!ch || !rings) { kprintf("[VMBUS] %s: out of memory\n", drv->name); return false; }
    vm_ring_init(&ch->out, rings, RING_PAGES);
    vm_ring_init(&ch->in, rings + RING_PAGES * VMBUS_PAGE, RING_PAGES);
    ch->drv = drv;
    ch->relid = o->relid;
    ch->conn_id = o->conn_id;
    ch->dedicated = o->dedicated;

    static UINT32 next_gpadl = 0xE1E10;
    ChGpadlHeader g;
    memset(&g, 0, sizeof(g));
    g.h.msgtype = CHMSG_GPADL_HEADER;
    g.child_relid = o->relid;
    g.gpadl = next_gpadl++;
    g.range_buflen = 8 + 8 * 2 * RING_PAGES;
    g.rangecount = 1;
    g.byte_count = 2 * RING_PAGES * VMBUS_PAGE;
    for (int i = 0; i < 2 * RING_PAGES; i++) g.pfn[i] = (phys(rings) >> 12) + i;
    UINT64 st = post_message(&g, sizeof(g));
    UINT32 got = st ? 0 : wait_message(msg, 1000);
    while (got && !(got == CHMSG_GPADL_CREATED && ((ChGpadlCreated *)msg)->gpadl == g.gpadl))
        got = wait_message(msg, 1000);
    if (!got || ((ChGpadlCreated *)msg)->status) {
        kprintf("[VMBUS] %s: the host did not take its rings (status 0x%lx/%u)\n", drv->name,
                st, got ? ((ChGpadlCreated *)msg)->status : 0);
        return false;
    }

    ChOpenChannel op;
    memset(&op, 0, sizeof(op));
    op.h.msgtype = CHMSG_OPENCHANNEL;
    op.child_relid = o->relid;
    op.openid = o->relid;
    op.gpadl = g.gpadl;
    op.target_vp = g_vp;
    op.downstream_offset = RING_PAGES;
    st = post_message(&op, sizeof(op));
    got = st ? 0 : wait_message(msg, 1000);
    while (got && !(got == CHMSG_OPENCHANNEL_RESULT && ((ChOpenResult *)msg)->openid == op.openid))
        got = wait_message(msg, 1000);
    if (!got || ((ChOpenResult *)msg)->status) {
        kprintf("[VMBUS] %s: the host did not open it (status 0x%lx/%u)\n", drv->name,
                st, got ? ((ChOpenResult *)msg)->status : 0);
        return false;
    }
    ch->in.hdr->interrupt_mask = 1;       /* polled */
    g_ch[g_nch++] = ch;
    ch->ctx = drv->opened(ch);
    kprintf("[VMBUS] %s: channel %u open\n", drv->name, o->relid);
    return true;
}

static void irq_ignored(void *ctx) { (void)ctx; }

bool VmbusInit(void)
{
    if (!(cpuid(1, 0).ecx & (1u << 31))) return false;            /* no hypervisor */
    CpuidResult id = cpuid(0x40000000, 0);
    if (id.ebx != 0x7263694D || id.ecx != 0x666F736F || id.edx != 0x76482074 || id.eax < 0x40000003)
        return false;                                              /* not "Microsoft Hv" */
    CpuidResult f = cpuid(0x40000003, 0);
    if (!(f.eax & HV_FEAT_SYNIC) || !(f.eax & HV_FEAT_HYPERCALL) ||
        !(f.ebx & HV_PRIV_POST_MESSAGE) || !(f.ebx & HV_PRIV_SIGNAL_EVENT)) {
        kprintf("[VMBUS] Hyper-V without the SynIC or its hypercalls: no VMBus\n");
        return false;
    }

    g_hypercall_page = kernel_alloc_pages(1);
    g_simp = kernel_alloc_pages(1);
    UINT8 *siefp = kernel_alloc_pages(1);
    g_post = kernel_alloc_pages(1);
    g_int_page = kernel_alloc_pages(1);
    if (!g_hypercall_page || !g_simp || !siefp || !g_post || !g_int_page) return false;
    memset(g_simp, 0, VMBUS_PAGE);
    memset(siefp, 0, VMBUS_PAGE);
    memset(g_int_page, 0, VMBUS_PAGE);

    wrmsr(HV_MSR_GUEST_OS_ID, NOVAOS_GUEST_ID);
    wrmsr(HV_MSR_HYPERCALL, phys(g_hypercall_page) | 1);
    if (!(rdmsr(HV_MSR_HYPERCALL) & 1)) {
        kprintf("[VMBUS] The hypervisor did not enable hypercalls\n");
        return false;
    }
    g_vp = (f.eax & HV_FEAT_VP_INDEX) ? (UINT32)rdmsr(HV_MSR_VP_INDEX) : 0;

    IrqInstall(IRQ_VMBUS, irq_ignored, NULL);   /* (the slot is polled; the interrupt only needs its EOI) */
    wrmsr(HV_MSR_SIMP, phys(g_simp) | 1);
    wrmsr(HV_MSR_SIEFP, phys(siefp) | 1);
    wrmsr(HV_MSR_SINT0 + VMBUS_SINT, IRQ_VMBUS);
    wrmsr(HV_MSR_SCONTROL, 1);

    if (!connect()) return false;

    UINT8 msg[240];
    ChHeader req = { CHMSG_REQUESTOFFERS, 0 };
    if (post_message(&req, sizeof(req))) { kprintf("[VMBUS] Cannot ask for the devices\n"); return false; }
    for (;;) {
        UINT32 got = wait_message(msg, 2000);
        if (!got || got == CHMSG_ALLOFFERS_DELIVERED) break;
        if (got != CHMSG_OFFERCHANNEL) continue;
        ChOffer *o = (ChOffer *)msg;
        char g[40];
        guid_text(o->if_type, g, sizeof(g));
        const char *name = "unknown";
        for (UINT32 k = 0; k < sizeof(g_known) / sizeof(g_known[0]); k++)
            if (!strcmp(g, g_known[k].guid)) name = g_known[k].name;
        kprintf("[VMBUS] Device %u: %s {%s}\n", o->child_relid, name, g);
        if (g_noffers < MAX_OFFERS) {
            Offer *s = &g_offers[g_noffers++];
            memcpy(s->type, o->if_type, 16);
            s->relid = o->child_relid;
            s->conn_id = o->connection_id;
            s->dedicated = o->is_dedicated_interrupt & 1;
        }
    }

    for (int i = 0; i < g_noffers; i++)
        for (UINT32 d = 0; d < sizeof(g_drivers) / sizeof(g_drivers[0]); d++)
            if (!memcmp(g_offers[i].type, g_drivers[d]->type, 16) && g_nch < MAX_CHANNELS)
                open_channel(&g_offers[i], g_drivers[d]);
    __atomic_store_n(&g_ready, true, __ATOMIC_RELEASE);
    return g_nch > 0;
}

static void signal_host(VmbusChannel *ch)
{
    if (!ch->dedicated)
        __atomic_fetch_or((volatile UINT32 *)(g_int_page + VMBUS_PAGE / 2) + ch->relid / 32,
                          1u << (ch->relid % 32), __ATOMIC_SEQ_CST);
    hv_call(HVCALL_SIGNAL_EVENT | HV_HYPERCALL_FAST, ch->conn_id, 0);
}

bool VmbusSend(VmbusChannel *ch, const void *data, UINT32 len, UINT64 trans_id, bool want_completion)
{
    bool sig = false;
    if (!vm_ring_write(&ch->out, data, len, trans_id,
                       want_completion ? VM_PKT_FLAG_COMPLETION_REQUESTED : 0, &sig))
        return false;
    if (sig) signal_host(ch);
    return true;
}

void VmbusPoll(void)
{
    if (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) return;
    for (int i = 0; i < g_nch; i++) {
        VmbusChannel *ch = g_ch[i];
        VmPacketDesc d;
        UINT32 len;
        bool sig = false, any = false;
        for (int n = 0; n < 64 && vm_ring_read(&ch->in, &d, ch->pkt, PKT_MAX, &len, &sig); n++) {
            any |= sig;
            if (d.type == VM_PKT_DATA_INBAND)
                ch->drv->packet(ch->ctx, ch->pkt, len < PKT_MAX ? len : PKT_MAX);
        }
        if (any) signal_host(ch);
    }
}
