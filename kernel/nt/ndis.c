/*
 * ndis.c — NovaOS's ndis.sys: the NDIS 6 host network miniports run on
 *
 * A Windows network adapter driver (virtio-win's netkvm.sys, a vendor's
 * Ethernet or Wi-Fi driver) is an NDIS miniport: it never sees an IRP.
 * DriverEntry hands its handlers to NdisMRegisterMiniportDriver, and this
 * file is the driver to the I/O and PnP managers on its behalf
 * (docs/design/windows-drivers.md: NovaOS writes its own NDIS host, it
 * never ships Microsoft's ndis.sys):
 *
 *   AddDevice      an FDO on the PCI function's PDO (pnp.c), and the
 *                  miniport's MiniportAddDevice
 *   START_DEVICE   passed down to the PDO first, then MiniportStartDevice,
 *                  MiniportInitializeEx with the translated resources,
 *                  MiniportRestart, and the packet filter (an OID set):
 *                  the adapter runs
 *   QUERY_REMOVE, REMOVE, SURPRISE_REMOVAL
 *                  MiniportPause, MiniportHaltEx, MiniportRemoveDevice
 *
 * Through the adapter's life the miniport calls back: its attributes
 * (MAC address, link state), its interrupt (a line-based one through
 * IoConnectInterruptEx: the ISR asks MiniportInterrupt, the DPC runs
 * MiniportInterruptDPC with receive throttling), its registers and shared
 * memory, scatter/gather DMA for frames it sends (the DMA adapter, dma.c),
 * NET_BUFFER_LIST pools, configuration (its driver key in the registry),
 * read/write locks, work items and events.
 *
 * The protocol side is NovaOS's own network stack (kernel/net/net.c,
 * lwIP), not an NDIS protocol driver: a running adapter is attached to it
 * as its network adapter (NetAttach).  Frames lwIP sends become one
 * NET_BUFFER_LIST each from a ring of send slots; frames the miniport
 * indicates are copied into a receive ring the net thread drains, and the
 * miniport's NET_BUFFER_LISTs go back to it once its DPC is done.  An NDIS
 * protocol driver a miniport registers (netkvm's failover protocol) is
 * accepted and never bound.
 *
 * NDIS 6.85 is what NdisGetVersion reports: without receive polling
 * (NdisRegisterPoll, 6.89), which a miniport then does not ask for.  One
 * adapter at a time is NovaOS's network adapter.
 */
#include "ntapi.h"
#include "ndis.h"
#include "wdi.h"
#include "wlan.h"
#include "../net/net.h"
#include "../hal/pci.h"
#include "../ke/irql.h"
#include "../ke/printf.h"
#include "../ke/scheduler.h"
#include "../lib/string.h"
#include "../mm/vmm.h"
#include "../um/um.h"

#define TAG          0x5349444E          /* 'NDIS' */
#define NDIS_VERSION 0x00060055          /* 6.85 */

#define M_DRIVER   0x7672444Eu           /* the handles' first fields */
#define M_ADAPTER  0x7041444Eu
#define M_CONTROL  0x7443444Eu
#define M_POOL     0x6C50444Eu
#define M_CONFIG   0x6643444Eu
#define M_PROTOCOL 0x7250444Eu
#define M_WORK     0x6B57444Eu

#define TX_SLOTS   32
#define TX_BUF     2048
#define RX_SLOTS   128
#define RX_FRAME   1536

#define SERVICES_KEY "Machine\\SYSTEM\\CurrentControlSet\\Services\\"
#define ENUM_KEY     "Machine\\SYSTEM\\CurrentControlSet\\Enum\\"
#define CLASS_KEY    "Machine\\SYSTEM\\CurrentControlSet\\Control\\Class\\"

typedef NDIS_STATUS (NT_API *SET_OPTIONS)(void *driver_handle, void *driver_ctx);
typedef NDIS_STATUS (NT_API *INITIALIZE)(void *adapter_handle, void *driver_ctx, NDIS_MINIPORT_INIT_PARAMETERS *);
typedef void        (NT_API *HALT)(void *ctx, int action);
typedef void        (NT_API *UNLOAD)(NT_DRIVER_OBJECT *);
typedef NDIS_STATUS (NT_API *PAUSE)(void *ctx, NDIS_MINIPORT_PAUSE_PARAMETERS *);
typedef NDIS_STATUS (NT_API *RESTART)(void *ctx, NDIS_MINIPORT_RESTART_PARAMETERS *);
typedef NDIS_STATUS (NT_API *OID_REQUEST)(void *ctx, NDIS_OID_REQUEST *);
typedef void        (NT_API *SEND)(void *ctx, NET_BUFFER_LIST *, UINT32 port, UINT32 flags);
typedef void        (NT_API *RETURN)(void *ctx, NET_BUFFER_LIST *, UINT32 flags);
typedef NDIS_STATUS (NT_API *ADD_DEVICE)(void *adapter_handle, void *driver_ctx);
typedef void        (NT_API *REMOVE_DEVICE)(void *add_ctx);
typedef NDIS_STATUS (NT_API *START_DEVICE)(void *add_ctx, NT_IRP *);
typedef UINT8       (NT_API *ISR)(void *ctx, UINT8 *queue_dpc, UINT32 *target);
typedef void        (NT_API *ISR_DPC)(void *ctx, void *dpc_ctx, void *throttle, void *reserved);
typedef void        (NT_API *INTR_ONOFF)(void *ctx);
typedef void        (NT_API *WORK_ROUTINE)(void *ctx, void *work_handle);
typedef UINT8       (NT_API *SYNC_FN)(void *ctx);

#define PAUSE_ADAPTER_REMOVED   0x10    /* NDIS_PAUSE_MINIPORT_DEVICE_REMOVE */
#define HALT_DEVICE_DISABLED    0       /* NdisHaltDeviceDisabled */
#define HALT_SURPRISE_REMOVED   3       /* NdisHaltDeviceSurpriseRemoved */

/* ---- handles ---- */

typedef struct NdisDriver {
    UINT32               magic;
    struct NdisDriver   *next;
    NT_DRIVER_OBJECT    *obj;
    void                *ctx;           /* MiniportDriverContext */
    NDIS_MINIPORT_DRIVER_CHARACTERISTICS ch;
    NDIS_MINIPORT_PNP_CHARACTERISTICS    pnp;
    bool                 has_pnp;
    bool                 wdi;           /* a WDI (Wi-Fi) miniport: NovaOS is its upper edge */
    NDIS_MINIPORT_DRIVER_WDI_CHARACTERISTICS wdi_ch;
    char                 service[64];
    NT_DRIVER_UNLOAD     unload;        /* (NDIS's, calling the miniport's) */
} NdisDriver;

typedef struct {
    NET_BUFFER_LIST *nbl;
    UINT8           *buf;
    NT_MDL          *mdl;
    volatile int     busy;
} TxSlot;

typedef struct NdisAdapter {
    UINT32               magic;
    NdisDriver          *drv;
    NT_DEVICE_OBJECT    *fdo, *pdo, *lower;
    void                *ctx;           /* MiniportAdapterContext (registration attributes) */
    void                *add_ctx;       /* MiniportAddDeviceContext */
    bool                 adding, initialized, running;
    NT_CM_RESOURCE_LIST *raw, *xlat;
    UINT8                mac[6];
    UINT32               mtu;
    volatile bool        link;
    /* interrupt */
    NDIS_MINIPORT_INTERRUPT_CHARACTERISTICS intr;
    void                *intr_ctx;
    void                *intr_obj;
    NT_KSPIN_LOCK        intr_lock;
    NT_KDPC              dpc;
    bool                 intr_on;
    /* scatter/gather DMA */
    NT_DMA_ADAPTER      *dma;
    void                *process_sg;
    /* an OID request NDIS itself makes */
    NT_KEVENT            oid_done;
    NDIS_STATUS          oid_status;
    NDIS_OID_REQUEST    *oid_waiting;
    /* frames received, for the net thread; NBLs to give back */
    NT_KSPIN_LOCK        rx_lock;
    UINT8               *rx;            /* RX_SLOTS frames of RX_FRAME */
    UINT16               rx_len[RX_SLOTS];
    UINT32               rx_head, rx_tail;
    NET_BUFFER_LIST     *returns;
    /* frames to send */
    TxSlot               tx[TX_SLOTS];
    UINT32               tx_next;
    UINT64               rx_frames, tx_frames, rx_dropped;
    UINT64               tx_done, isrs, dpcs, sgs, sgs_freed;   /* (for the log when sending stalls) */
    bool                 tx_told;
    bool                 dpc_held;      /* the receive copies are full: the DPC waits for the stack */
    void                *dpc_held_ctx;
    UINT64               dpc_log;       /* (the DPC count of the next progress line) */
    /* a WDI miniport's adapter (wdi_start): what its allocate and open were given */
    NDIS_MINIPORT_INIT_PARAMETERS init;
    NDIS_WDI_INIT_PARAMETERS      wdi_init;
    bool                 wdi_allocated, wdi_open;
    NT_KEVENT            wdi_done;
    NDIS_STATUS          wdi_status;
    WdiHost             *wdi_host;      /* its command layer (wdi.c), once open */
} NdisAdapter;

/* A control device (NdisRegisterDeviceEx): the miniport's own dispatch
 * table; the extension it asked for follows */
typedef struct {
    UINT32               magic;
    NdisDriver          *drv;
    NT_DEVICE_OBJECT    *dev;
    NT_UNICODE_STRING    link;
    NT_WCHAR             link_buf[128];
    NT_DRIVER_DISPATCH   major[NT_IRP_MJ_MAXIMUM_FUNCTION + 1];
    UINT8                reserved[] __attribute__((aligned(16)));
} NdisControl;

typedef struct {
    UINT32               magic;
    NET_BUFFER_LIST_POOL_PARAMETERS p;
} NdisPool;

/* An NBL as NDIS allocates it: with its first NET_BUFFER, and the data
 * buffer and MDL a pool with a DataSize gives it */
typedef struct {
    void                *data;
    NT_MDL              *mdl;
    bool                 clone;
    NET_BUFFER_LIST      nbl;
    NET_BUFFER           nb;
} NblBlock;

#define BLOCK_OF(n) ((NblBlock *)((UINT8 *)(n) - __builtin_offsetof(NblBlock, nbl)))

typedef struct NdisParam {
    struct NdisParam    *next;
    NDIS_CONFIGURATION_PARAMETER p;
    NT_WCHAR             text[];
} NdisParam;

typedef struct NdisConfig {
    UINT32               magic;
    char                 key[300];
    NdisParam           *params;
    struct NdisConfig   *subkeys;
    struct NdisConfig   *next_sub;
} NdisConfig;

typedef struct {
    UINT32               magic;
    void                *ctx;
} NdisProtocol;

typedef struct {
    UINT32               magic;
    NT_IO_WORKITEM      *item;
    WORK_ROUTINE         routine;
    void                *ctx;
} NdisWork;

static NdisDriver   *g_drivers;
static UINT8         g_binding;         /* the network stack's binding: its frames' SourceHandle */
static NdisAdapter  *g_net;             /* the adapter net.c uses */
static NT_KSPIN_LOCK g_lock;

static void *pool_alloc(UINT64 n) { void *p = nt_ExAllocatePoolWithTag(0, n, TAG); if (p) memset(p, 0, n); return p; }
static void  pool_free(void *p)   { if (p) nt_ExFreePool(p); }

static NdisAdapter *adapter_of(void *h)
{
    NdisAdapter *a = h;
    return a && a->magic == M_ADAPTER ? a : NULL;
}

static NdisDriver *driver_of_object(NT_DRIVER_OBJECT *obj)
{
    for (NdisDriver *d = g_drivers; d; d = d->next) if (d->obj == obj) return d;
    return NULL;
}

/* A kernel address for an MDL's buffer */
static UINT8 *mdl_va(NT_MDL *m)
{
    if (m->MdlFlags & (NT_MDL_MAPPED_TO_SYSTEM_VA | NT_MDL_SOURCE_IS_NONPAGED_POOL)) return m->MappedSystemVa;
    return (UINT8 *)m->StartVa + m->ByteOffset;
}

/* -----------------------------------------------------------------------
 * The net.c side: one adapter is NovaOS's network adapter
 * ----------------------------------------------------------------------- */
static void return_pending(NdisAdapter *a);
static UINT32 rx_room(NdisAdapter *a);

static const char *net_name(void) { return "Windows network adapter (NDIS)"; }
static void net_mac(UINT8 mac[6]) { if (g_net) memcpy(mac, g_net->mac, 6); }
static bool net_link(void) { return g_net && g_net->running && g_net->link; }

static bool net_transmit(const void *frame, UINT16 len)
{
    NdisAdapter *a = g_net;
    if (!a || !a->running || len > TX_BUF) return false;
    return_pending(a);
    TxSlot *s = NULL;
    for (int i = 0; i < TX_SLOTS && !s; i++) {
        TxSlot *t = &a->tx[(a->tx_next + (UINT32)i) % TX_SLOTS];
        if (!__atomic_exchange_n(&t->busy, 1, __ATOMIC_ACQUIRE)) s = t;
    }
    if (!s) {
        if (!a->tx_told) {
            a->tx_told = true;
            kprintf("[NDIS] %s: every send slot is waiting for the miniport (%llu sent, %llu completed; "
                    "%llu interrupts, %llu DPCs, %llu frames received, %llu of %llu S/G lists freed)\n", a->drv->service,
                    (unsigned long long)a->tx_frames, (unsigned long long)a->tx_done, (unsigned long long)a->isrs,
                    (unsigned long long)a->dpcs, (unsigned long long)a->rx_frames,
                    (unsigned long long)a->sgs_freed, (unsigned long long)a->sgs);
        }
        return false;
    }
    a->tx_next = (UINT32)(s - a->tx + 1) % TX_SLOTS;
    memcpy(s->buf, frame, len);
    s->mdl->ByteCount = len;
    NET_BUFFER_LIST *nbl = s->nbl;
    NET_BUFFER *nb = nbl->FirstNetBuffer;
    nbl->Next = NULL;
    nbl->Status = NDIS_STATUS_SUCCESS;
    nbl->SourceHandle = &g_binding;    /* (the sender's binding, as NDIS sets it: never the miniport's own handle,
                                        * which netkvm takes for a frame of its own and does not complete) */
    memset(nbl->NetBufferListInfo, 0, sizeof(nbl->NetBufferListInfo));
    nb->Next = NULL;
    nb->MdlChain = nb->CurrentMdl = s->mdl;
    nb->DataOffset = nb->CurrentMdlOffset = 0;
    nb->DataLength = len;
    a->tx_frames++;
    UINT8 old = nt_KfRaiseIrql(DISPATCH_LEVEL);
    ((SEND)a->drv->ch.SendNetBufferListsHandler)(a->ctx, nbl, 0, NDIS_SEND_FLAGS_DISPATCH_LEVEL);
    nt_KeLowerIrql(old);
    return true;
}

static int net_receive(void *buf, int cap)
{
    NdisAdapter *a = g_net;
    if (!a) return 0;
    return_pending(a);
    UINT8 old = nt_KeAcquireSpinLockRaiseToDpc(&a->rx_lock);
    int n = 0;
    if (a->rx_tail != a->rx_head) {
        UINT32 k = a->rx_tail % RX_SLOTS;
        n = a->rx_len[k] < cap ? a->rx_len[k] : cap;
        memcpy(buf, a->rx + (UINT64)k * RX_FRAME, (size_t)n);
        a->rx_tail++;
        if (a->dpc_held && rx_room(a) >= RX_SLOTS / 2) {    /* (room again: the DPC goes on) */
            a->dpc_held = false;
            nt_KeInsertQueueDpc(&a->dpc, a->dpc_held_ctx, NULL);
        }
    }
    nt_KeReleaseSpinLock(&a->rx_lock, old);
    return n;
}

static const NetAdapter g_adapter_ops = { net_name, net_mac, net_link, net_transmit, net_receive };

/* -----------------------------------------------------------------------
 * The I/O manager's side: AddDevice and the IRPs
 * ----------------------------------------------------------------------- */
static NT_API NT_STATUS sync_done(NT_DEVICE_OBJECT *dev, NT_IRP *irp, void *ctx)
{
    (void)dev; (void)irp;
    nt_KeSetEvent(ctx, 0, 0);
    return NT_STATUS_MORE_PROCESSING_REQUIRED;
}

/* Pass @irp down to the PDO and wait for it: its status (the IRP is ours again) */
static NT_STATUS forward_and_wait(NdisAdapter *a, NT_IRP *irp)
{
    NT_KEVENT ev;
    nt_KeInitializeEvent(&ev, NT_NotificationEvent, 0);
    NT_IO_STACK_LOCATION *cur = NtIrpCurrentStack(irp), *next = NtIrpNextStack(irp);
    memcpy(next, cur, __builtin_offsetof(NT_IO_STACK_LOCATION, CompletionRoutine));
    next->Control = 0;
    next->CompletionRoutine = sync_done;
    next->Context = &ev;
    next->Control = NT_SL_INVOKE_ON_SUCCESS | NT_SL_INVOKE_ON_ERROR | NT_SL_INVOKE_ON_CANCEL;
    if (nt_IofCallDriver(a->lower, irp) == NT_STATUS_PENDING) nt_KeWaitForSingleObject(&ev, 0, 0, 0, NULL);
    return irp->IoStatus.Status;
}

static NT_STATUS forward(NdisAdapter *a, NT_IRP *irp)
{
    irp->CurrentLocation++;                                  /* IoSkipCurrentIrpStackLocation */
    irp->Tail.Overlay.CurrentStackLocation++;
    return nt_IofCallDriver(a->lower, irp);
}

static NT_STATUS complete(NT_IRP *irp, NT_STATUS st)
{
    irp->IoStatus.Status = st;
    nt_IofCompleteRequest(irp, 0);
    return st;
}

/* An OID request NDIS makes of the miniport, waited for */
static NDIS_STATUS oid_set(NdisAdapter *a, UINT32 oid, void *buf, UINT32 len)
{
    NDIS_OID_REQUEST *r = pool_alloc(sizeof(*r));
    if (!r) return NDIS_STATUS_RESOURCES;
    r->Header.Type = NDIS_OBJECT_TYPE_OID_REQUEST;
    r->Header.Revision = 1;
    r->Header.Size = sizeof(*r);
    r->RequestType = NdisRequestSetInformation;
    r->DATA.SET_INFORMATION.Oid = oid;
    r->DATA.SET_INFORMATION.InformationBuffer = buf;
    r->DATA.SET_INFORMATION.InformationBufferLength = len;
    nt_KeClearEvent(&a->oid_done);
    a->oid_waiting = r;
    NDIS_STATUS st = ((OID_REQUEST)a->drv->ch.OidRequestHandler)(a->ctx, r);
    if (st == NDIS_STATUS_PENDING) {
        nt_KeWaitForSingleObject(&a->oid_done, 0, 0, 0, NULL);
        st = a->oid_status;
    }
    a->oid_waiting = NULL;
    pool_free(r);
    return st;
}

static void tx_free(NdisAdapter *a)
{
    for (int i = 0; i < TX_SLOTS; i++) {
        TxSlot *s = &a->tx[i];
        if (s->mdl) nt_IoFreeMdl(s->mdl);
        if (s->nbl) pool_free(BLOCK_OF(s->nbl));
        pool_free(s->buf);
        memset(s, 0, sizeof(*s));
    }
    kfree(a->rx);
    a->rx = NULL;
}

static bool tx_setup(NdisAdapter *a)
{
    a->rx = kzalloc((size_t)RX_SLOTS * RX_FRAME);
    if (!a->rx) return false;
    for (int i = 0; i < TX_SLOTS; i++) {
        TxSlot *s = &a->tx[i];
        NblBlock *b = pool_alloc(sizeof(*b));
        s->buf = pool_alloc(TX_BUF);
        if (!b || !s->buf) { pool_free(b); return false; }
        s->nbl = &b->nbl;
        b->nbl.FirstNetBuffer = &b->nb;
        b->nbl.ProtocolReserved[0] = s;
        s->mdl = nt_IoAllocateMdl(s->buf, TX_BUF, 0, 0, NULL);
        if (!s->mdl) return false;
        nt_MmBuildMdlForNonPagedPool(s->mdl);
    }
    return true;
}

/* -----------------------------------------------------------------------
 * WDI: a Wi-Fi miniport's adapter.  On Windows the WDI upper edge
 * (wdiwifi.sys) is the NDIS miniport for it and drives the IHV driver's
 * lower edge: MiniportWdiAllocateAdapter, then MiniportWdiOpenAdapter,
 * the IHV's "open task" (its hardware start: registers, firmware), which
 * completes through NdisWdiOpenAdapterComplete; then WDI commands (TLV
 * messages in NdisRequestMethod OID requests to its MiniportOidRequest:
 * adapter capabilities, ports, scans, connections), which are wdi.c's,
 * the command layer (roadmap step 28.1, docs/drivers.md).
 * ----------------------------------------------------------------------- */
typedef NDIS_STATUS (NT_API *WDI_ALLOCATE)(void *adapter_handle, void *driver_ctx, NDIS_MINIPORT_INIT_PARAMETERS *,
                                          NDIS_WDI_INIT_PARAMETERS *, NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES *);
typedef void        (NT_API *WDI_FREE)(void *ctx);
typedef NDIS_STATUS (NT_API *WDI_OPEN)(void *ctx, NDIS_MINIPORT_INIT_PARAMETERS *);
typedef NDIS_STATUS (NT_API *WDI_CLOSE)(void *ctx);

#define WDI_OPEN_WAIT_S 60

static NT_API void wdi_complete(void *h, NDIS_STATUS st)
{
    NdisAdapter *a = adapter_of(h);
    if (!a) return;
    a->wdi_status = st;
    nt_KeSetEvent(&a->wdi_done, 0, 0);
}

static NT_API void wdi_idle(void *h, UINT32 state) { (void)h; (void)state; }   /* (NovaOS never idles the adapter) */

/* Run a lower-edge call that completes through wdi_complete; its status */
static NDIS_STATUS wdi_wait(NdisAdapter *a, NDIS_STATUS st, const char *what)
{
    if (st != NDIS_STATUS_SUCCESS && st != NDIS_STATUS_PENDING) return st;
    INT64 t = -(INT64)WDI_OPEN_WAIT_S * 10000000;
    if (nt_KeWaitForSingleObject(&a->wdi_done, 0, 0, 0, &t) != NT_STATUS_SUCCESS) {
        kprintf("[NDIS] %s: WDI: %s did not complete in %d s\n", a->drv->service, what, WDI_OPEN_WAIT_S);
        return NDIS_STATUS_FAILURE;
    }
    return a->wdi_status;
}

#define WDI_REQUEST_WAIT_S 60

/* wdi.c's command: an OID_WDI_* method request, waited for (also when the
 * miniport pends it and completes it with NdisMOidRequestComplete) */
static INT32 wdi_request(void *h, UINT32 oid, UINT16 port, void *buf, UINT32 in_len, UINT32 out_len, UINT32 *written)
{
    NdisAdapter *a = adapter_of(h);
    *written = 0;
    if (!a || !a->wdi_open) return NDIS_STATUS_FAILURE;
    NDIS_OID_REQUEST *r = pool_alloc(sizeof(*r));
    if (!r) return NDIS_STATUS_RESOURCES;
    r->Header.Type = NDIS_OBJECT_TYPE_OID_REQUEST;
    r->Header.Revision = 1;
    r->Header.Size = sizeof(*r);
    r->RequestType = NdisRequestMethod;
    r->PortNumber = port == WDI_PORT_ID_ADAPTER ? 0 : port;
    r->DATA.METHOD_INFORMATION.Oid = oid;
    r->DATA.METHOD_INFORMATION.InformationBuffer = buf;
    r->DATA.METHOD_INFORMATION.InputBufferLength = in_len;
    r->DATA.METHOD_INFORMATION.OutputBufferLength = out_len;
    nt_KeClearEvent(&a->oid_done);
    a->oid_waiting = r;
    NDIS_STATUS st = ((OID_REQUEST)a->drv->ch.OidRequestHandler)(a->ctx, r);
    if (st == NDIS_STATUS_PENDING) {
        INT64 t = -(INT64)WDI_REQUEST_WAIT_S * 10000000;
        if (nt_KeWaitForSingleObject(&a->oid_done, 0, 0, 0, &t) != NT_STATUS_SUCCESS) {
            kprintf("[NDIS] %s: WDI: OID 0x%08x not completed in %d s\n", a->drv->service, oid, WDI_REQUEST_WAIT_S);
            a->oid_waiting = NULL;      /* (the request is left to the miniport) */
            return NDIS_STATUS_FAILURE;
        }
        st = a->oid_status;
    }
    a->oid_waiting = NULL;
    *written = r->DATA.METHOD_INFORMATION.BytesWritten;
    pool_free(r);
    return st;
}

static NT_STATUS wdi_start(NdisAdapter *a)
{
    NdisDriver *d = a->drv;
    NDIS_WDI_INIT_PARAMETERS *w = &a->wdi_init;
    memset(w, 0, sizeof(*w));
    w->Header.Type = NDIS_OBJECT_TYPE_WDI_INIT_PARAMETERS;
    w->Header.Revision = 1;
    w->Header.Size = sizeof(*w);
    w->WdiVersion = d->wdi_ch.WdiVersion;
    w->OpenAdapterCompleteHandler = (void *)wdi_complete;
    w->CloseAdapterCompleteHandler = (void *)wdi_complete;
    w->UeIdleNotificationConfirm = (void *)wdi_idle;
    w->UeIdleNotificationComplete = (void *)wdi_idle;
    NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES ra;
    memset(&ra, 0, sizeof(ra));
    ra.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES;
    ra.Header.Revision = 2;
    ra.Header.Size = sizeof(ra);
    kprintf("[NDIS] %s: WDI: MiniportWdiAllocateAdapter\n", d->service);
    NDIS_STATUS ns = ((WDI_ALLOCATE)d->wdi_ch.AllocateAdapterHandler)(a, d->ctx, &a->init, w, &ra);
    if (ns != NDIS_STATUS_SUCCESS) {
        kprintf("[NDIS] %s: WDI: MiniportWdiAllocateAdapter failed (0x%08x)\n", d->service, (UINT32)ns);
        return NT_STATUS_UNSUCCESSFUL;
    }
    a->ctx = ra.MiniportAdapterContext;
    a->wdi_allocated = true;
    nt_KeInitializeEvent(&a->wdi_done, NT_NotificationEvent, 0);
    kprintf("[NDIS] %s: WDI: MiniportWdiOpenAdapter (the driver starts its hardware)\n", d->service);
    ns = wdi_wait(a, ((WDI_OPEN)d->wdi_ch.OpenAdapterHandler)(a->ctx, &a->init), "MiniportWdiOpenAdapter");
    kprintf("[NDIS] %s: WDI: open adapter completed (0x%08x)\n", d->service, (UINT32)ns);
    if (ns != NDIS_STATUS_SUCCESS) {
        ((WDI_FREE)d->wdi_ch.FreeAdapterHandler)(a->ctx);
        a->wdi_allocated = false;
        return NT_STATUS_UNSUCCESSFUL;
    }
    a->wdi_open = true;
    a->initialized = true;
    /* (the device's description, for netsh wlan show interfaces: "@inf,%key%;text" or plain) */
    char ek[200], desc[160] = "";
    const char *inst = NtPnpInstanceOf(a->pdo);
    ksnprintf(ek, sizeof(ek), ENUM_KEY "%s", inst ? inst : "");
    if (inst) um_registry_get_sz(ek, "DeviceDesc", desc, sizeof(desc));
    const char *semi = strrchr(desc, ';');
    a->wdi_host = WdiAttach(a, d->service, semi ? semi + 1 : desc, wdi_request);
    return NT_STATUS_SUCCESS;
}

static void wdi_stop(NdisAdapter *a)
{
    NdisDriver *d = a->drv;
    if (a->wdi_host) {
        WdiDetach(a->wdi_host);
        a->wdi_host = NULL;
    }
    if (a->wdi_open && d->wdi_ch.CloseAdapterHandler) {
        nt_KeClearEvent(&a->wdi_done);
        NDIS_STATUS ns = wdi_wait(a, ((WDI_CLOSE)d->wdi_ch.CloseAdapterHandler)(a->ctx), "MiniportWdiCloseAdapter");
        kprintf("[NDIS] %s: WDI: adapter closed (0x%08x)\n", d->service, (UINT32)ns);
    }
    a->wdi_open = false;
    if (a->wdi_allocated) ((WDI_FREE)d->wdi_ch.FreeAdapterHandler)(a->ctx);
    a->wdi_allocated = false;
    a->initialized = false;
}

static void halt(NdisAdapter *a, bool surprise)
{
    if (a->drv->wdi) { wdi_stop(a); return; }
    if (!a->initialized) return;
    if (g_net == a) {
        NetDetach(&g_adapter_ops);
        g_net = NULL;
    }
    if (a->running) {
        a->running = false;
        NDIS_MINIPORT_PAUSE_PARAMETERS p = { { NDIS_OBJECT_TYPE_DEFAULT, 1, sizeof(p) }, 0, PAUSE_ADAPTER_REMOVED };
        NDIS_STATUS st = ((PAUSE)a->drv->ch.PauseHandler)(a->ctx, &p);
        if (st == NDIS_STATUS_PENDING) nt_KeDelayExecutionThread(0, 0, &(INT64){ -10000000 });   /* (1 s) */
    }
    return_pending(a);
    ((HALT)a->drv->ch.HaltHandlerEx)(a->ctx, surprise ? HALT_SURPRISE_REMOVED : HALT_DEVICE_DISABLED);
    a->initialized = false;
    kprintf("[NDIS] %s: halted (%llu frames received, %llu sent)\n", a->drv->service,
            (unsigned long long)a->rx_frames, (unsigned long long)a->tx_frames);
    tx_free(a);
}

static NT_STATUS start(NdisAdapter *a, NT_IRP *irp)
{
    NT_STATUS st = forward_and_wait(a, irp);
    if (!NT_SUCCESS_(st)) return st;
    NT_IO_STACK_LOCATION *sl = NtIrpCurrentStack(irp);
    a->raw = sl->Parameters.StartDevice.AllocatedResources;
    a->xlat = sl->Parameters.StartDevice.AllocatedResourcesTranslated;
    if (a->drv->has_pnp && a->drv->pnp.MiniportStartDeviceHandler) {
        NDIS_STATUS ns = ((START_DEVICE)a->drv->pnp.MiniportStartDeviceHandler)(a->add_ctx, irp);
        if (ns != NDIS_STATUS_SUCCESS) {
            kprintf("[NDIS] %s: MiniportStartDevice failed (0x%08x)\n", a->drv->service, (UINT32)ns);
            return NT_STATUS_UNSUCCESSFUL;
        }
    }
    NDIS_MINIPORT_INIT_PARAMETERS *ip = &a->init;
    memset(ip, 0, sizeof(*ip));
    ip->Header.Type = NDIS_OBJECT_TYPE_MINIPORT_INIT_PARAMETERS;
    ip->Header.Revision = 1;
    ip->Header.Size = sizeof(*ip);
    ip->AllocatedResources = a->xlat ? &a->xlat->List[0].PartialResourceList : NULL;
    ip->MiniportAddDeviceContext = a->add_ctx;
    ip->IfIndex = 1;
    ip->NetLuid = (a->drv->wdi ? 71ull : 6ull) << 48 | 1ull << 24;  /* (IF_TYPE_IEEE80211 or _ETHERNET_CSMACD, index 1) */
    if (a->drv->wdi) return wdi_start(a);
    if (!tx_setup(a)) { tx_free(a); return NT_STATUS_INSUFFICIENT_RESOURCES; }

    kprintf("[NDIS] %s: MiniportInitializeEx\n", a->drv->service);
    NDIS_STATUS ns = ((INITIALIZE)a->drv->ch.InitializeHandlerEx)(a, a->drv->ctx, ip);
    if (ns != NDIS_STATUS_SUCCESS) {
        kprintf("[NDIS] %s: MiniportInitializeEx failed (0x%08x)\n", a->drv->service, (UINT32)ns);
        tx_free(a);
        return NT_STATUS_UNSUCCESSFUL;
    }
    a->initialized = true;
    NDIS_MINIPORT_RESTART_PARAMETERS rp;
    memset(&rp, 0, sizeof(rp));
    rp.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    rp.Header.Revision = 1;
    rp.Header.Size = sizeof(rp);
    ns = ((RESTART)a->drv->ch.RestartHandler)(a->ctx, &rp);
    if (ns != NDIS_STATUS_SUCCESS && ns != NDIS_STATUS_PENDING) {
        kprintf("[NDIS] %s: MiniportRestart failed (0x%08x)\n", a->drv->service, (UINT32)ns);
        halt(a, false);
        return NT_STATUS_UNSUCCESSFUL;
    }
    a->running = true;
    UINT32 filter = NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_MULTICAST | NDIS_PACKET_TYPE_ALL_MULTICAST |
                    NDIS_PACKET_TYPE_BROADCAST;
    ns = oid_set(a, OID_GEN_CURRENT_PACKET_FILTER, &filter, sizeof(filter));
    if (ns != NDIS_STATUS_SUCCESS) kprintf("[NDIS] %s: the packet filter was refused (0x%08x)\n", a->drv->service, (UINT32)ns);
    kprintf("[NDIS] %s: running, MAC %02x:%02x:%02x:%02x:%02x:%02x, MTU %u, link %s\n", a->drv->service, a->mac[0],
            a->mac[1], a->mac[2], a->mac[3], a->mac[4], a->mac[5], a->mtu, a->link ? "up" : "down");
    if (!g_net) {
        g_net = a;
        NetAttach(&g_adapter_ops);
    }
    return NT_STATUS_SUCCESS;
}

static void remove_device(NdisAdapter *a)
{
    if (a->drv->has_pnp && a->drv->pnp.MiniportRemoveDeviceHandler)
        ((REMOVE_DEVICE)a->drv->pnp.MiniportRemoveDeviceHandler)(a->add_ctx);
}

static NT_STATUS adapter_pnp(NdisAdapter *a, NT_IRP *irp, NT_IO_STACK_LOCATION *sl)
{
    switch (sl->MinorFunction) {
    case NT_IRP_MN_START_DEVICE:
        return complete(irp, start(a, irp));
    case NT_IRP_MN_QUERY_REMOVE_DEVICE:
    case NT_IRP_MN_QUERY_STOP_DEVICE:
    case NT_IRP_MN_CANCEL_REMOVE_DEVICE:
    case NT_IRP_MN_CANCEL_STOP_DEVICE:
        irp->IoStatus.Status = NT_STATUS_SUCCESS;
        return forward(a, irp);
    case NT_IRP_MN_SURPRISE_REMOVAL:
        halt(a, true);
        irp->IoStatus.Status = NT_STATUS_SUCCESS;
        return forward(a, irp);
    case NT_IRP_MN_REMOVE_DEVICE: {
        halt(a, false);
        remove_device(a);
        irp->IoStatus.Status = NT_STATUS_SUCCESS;
        NT_STATUS st = forward(a, irp);
        NT_DEVICE_OBJECT *fdo = a->fdo;
        a->magic = 0;
        nt_IoDetachDevice(a->lower);
        nt_IoDeleteDevice(fdo);
        return st;
    }
    default:
        return forward(a, irp);
    }
}

static NT_API NT_STATUS dispatch(NT_DEVICE_OBJECT *dev, NT_IRP *irp)
{
    NT_IO_STACK_LOCATION *sl = NtIrpCurrentStack(irp);
    UINT32 magic = dev->DeviceExtension ? *(UINT32 *)dev->DeviceExtension : 0;
    if (magic == M_CONTROL) {                                /* the miniport's own device */
        NdisControl *c = dev->DeviceExtension;
        NT_DRIVER_DISPATCH fn = c->major[sl->MajorFunction];
        if (fn) return fn(dev, irp);
        return complete(irp, sl->MajorFunction == NT_IRP_MJ_CREATE || sl->MajorFunction == NT_IRP_MJ_CLOSE ||
                                     sl->MajorFunction == NT_IRP_MJ_CLEANUP
                                 ? NT_STATUS_SUCCESS
                                 : NT_STATUS_INVALID_DEVICE_REQUEST);
    }
    NdisAdapter *a = magic == M_ADAPTER ? dev->DeviceExtension : NULL;
    if (!a) return complete(irp, NT_STATUS_INVALID_DEVICE_REQUEST);
    switch (sl->MajorFunction) {
    case NT_IRP_MJ_PNP:            return adapter_pnp(a, irp, sl);
    case NT_IRP_MJ_POWER:
    case NT_IRP_MJ_SYSTEM_CONTROL: return forward(a, irp);
    case NT_IRP_MJ_CREATE:
    case NT_IRP_MJ_CLOSE:
    case NT_IRP_MJ_CLEANUP:        return complete(irp, NT_STATUS_SUCCESS);
    default:                       return complete(irp, NT_STATUS_INVALID_DEVICE_REQUEST);
    }
}

static NT_API NT_STATUS add_device(NT_DRIVER_OBJECT *obj, NT_DEVICE_OBJECT *pdo)
{
    NdisDriver *drv = driver_of_object(obj);
    if (!drv) return NT_STATUS_INVALID_DEVICE_REQUEST;
    NT_DEVICE_OBJECT *fdo;
    NT_STATUS st = nt_IoCreateDevice(obj, sizeof(NdisAdapter), NULL, 0x17 /* FILE_DEVICE_PHYSICAL_NETCARD */, 0, 0, &fdo);
    if (!NT_SUCCESS_(st)) return st;
    NdisAdapter *a = fdo->DeviceExtension;
    a->magic = M_ADAPTER;
    a->drv = drv;
    a->fdo = fdo;
    a->pdo = pdo;
    nt_KeInitializeEvent(&a->oid_done, NT_NotificationEvent, 0);
    a->lower = nt_IoAttachDeviceToDeviceStack(fdo, pdo);
    if (!a->lower) { nt_IoDeleteDevice(fdo); return NT_STATUS_UNSUCCESSFUL; }
    a->add_ctx = a;
    if (drv->has_pnp && drv->pnp.MiniportAddDeviceHandler) {
        a->adding = true;
        NDIS_STATUS ns = ((ADD_DEVICE)drv->pnp.MiniportAddDeviceHandler)(a, drv->ctx);
        a->adding = false;
        if (ns != NDIS_STATUS_SUCCESS) {
            kprintf("[NDIS] %s: MiniportAddDevice failed (0x%08x)\n", drv->service, (UINT32)ns);
            a->magic = 0;
            nt_IoDetachDevice(a->lower);
            nt_IoDeleteDevice(fdo);
            return NT_STATUS_UNSUCCESSFUL;
        }
    }
    fdo->Flags &= ~NT_DO_DEVICE_INITIALIZING;
    return NT_STATUS_SUCCESS;
}

static NT_API void driver_unload(NT_DRIVER_OBJECT *obj)
{
    NdisDriver *d = driver_of_object(obj);
    if (d && d->ch.UnloadHandler) ((UNLOAD)d->ch.UnloadHandler)(obj);
}

/* -----------------------------------------------------------------------
 * Registration
 * ----------------------------------------------------------------------- */
NT_API UINT32 nt_NdisGetVersion(void) { return NDIS_VERSION; }

static NDIS_STATUS register_driver(NT_DRIVER_OBJECT *obj, void *ctx, NDIS_MINIPORT_DRIVER_CHARACTERISTICS *ch,
                                   NDIS_MINIPORT_DRIVER_WDI_CHARACTERISTICS *wdi, void **out)
{
    NdisDriver *d = pool_alloc(sizeof(*d));
    if (!d) return NDIS_STATUS_RESOURCES;
    d->magic = M_DRIVER;
    d->obj = obj;
    d->ctx = ctx;
    UINT32 n = ch->Header.Size < sizeof(d->ch) ? ch->Header.Size : sizeof(d->ch);
    memcpy(&d->ch, ch, n);
    if (wdi) {
        d->wdi = true;
        n = wdi->Header.Size < sizeof(d->wdi_ch) ? wdi->Header.Size : sizeof(d->wdi_ch);
        memcpy(&d->wdi_ch, wdi, n);
    }
    NtDriver *nd = NtIoDriverOf(obj);
    strncpy(d->service, nd ? nd->service : "miniport", sizeof(d->service) - 1);
    UINT8 old = nt_KeAcquireSpinLockRaiseToDpc(&g_lock);
    d->next = g_drivers;
    g_drivers = d;
    nt_KeReleaseSpinLock(&g_lock, old);
    obj->DriverExtension->AddDevice = add_device;
    for (int i = 0; i <= NT_IRP_MJ_MAXIMUM_FUNCTION; i++) obj->MajorFunction[i] = dispatch;
    obj->DriverUnload = driver_unload;
    if (wdi)
        kprintf("[NDIS] %s: WDI miniport registered (NDIS %u.%u, WDI version 0x%x)\n", d->service, ch->MajorNdisVersion,
                ch->MinorNdisVersion, wdi->WdiVersion);
    else
        kprintf("[NDIS] %s: NDIS %u.%u miniport registered\n", d->service, ch->MajorNdisVersion, ch->MinorNdisVersion);
    if (ch->SetOptionsHandler) {
        NDIS_STATUS st = ((SET_OPTIONS)ch->SetOptionsHandler)(d, ctx);
        if (st != NDIS_STATUS_SUCCESS) {
            kprintf("[NDIS] %s: MiniportSetOptions failed (0x%08x)\n", d->service, (UINT32)st);
            return st;
        }
    }
    *out = d;
    return NDIS_STATUS_SUCCESS;
}

NT_API NDIS_STATUS nt_NdisMRegisterMiniportDriver(NT_DRIVER_OBJECT *obj, NT_UNICODE_STRING *reg, void *ctx,
                                                  NDIS_MINIPORT_DRIVER_CHARACTERISTICS *ch, void **out)
{
    (void)reg;
    if (!obj || !ch || !out || ch->Header.Type != NDIS_OBJECT_TYPE_MINIPORT_DRIVER_CHARACTERISTICS)
        return NDIS_STATUS_INVALID_PARAMETER;
    if (!ch->InitializeHandlerEx || !ch->HaltHandlerEx || !ch->PauseHandler || !ch->RestartHandler ||
        !ch->OidRequestHandler || !ch->SendNetBufferListsHandler || !ch->ReturnNetBufferListsHandler)
        return NDIS_STATUS_INVALID_PARAMETER;
    return register_driver(obj, ctx, ch, NULL, out);
}

/* A WDI miniport (Intel's Netwtw*.sys): the lower edge's handlers, which
 * NovaOS's upper edge (wdi_start) drives in place of the NDIS ones */
NT_API NDIS_STATUS nt_NdisMRegisterWdiMiniportDriver(NT_DRIVER_OBJECT *obj, NT_UNICODE_STRING *reg, void *ctx,
                                                     NDIS_MINIPORT_DRIVER_CHARACTERISTICS *ch,
                                                     NDIS_MINIPORT_DRIVER_WDI_CHARACTERISTICS *wdi, void **out)
{
    (void)reg;
    if (!obj || !ch || !wdi || !out || ch->Header.Type != NDIS_OBJECT_TYPE_MINIPORT_DRIVER_CHARACTERISTICS ||
        wdi->Header.Type != NDIS_OBJECT_TYPE_MINIPORT_WDI_CHARACTERISTICS)
        return NDIS_STATUS_INVALID_PARAMETER;
    if (!wdi->AllocateAdapterHandler || !wdi->FreeAdapterHandler || !wdi->OpenAdapterHandler ||
        !ch->OidRequestHandler)
        return NDIS_STATUS_INVALID_PARAMETER;
    return register_driver(obj, ctx, ch, wdi, out);
}

NT_API void nt_NdisMDeregisterMiniportDriver(void *h)
{
    NdisDriver *d = h;
    if (!d || d->magic != M_DRIVER) return;
    UINT8 old = nt_KeAcquireSpinLockRaiseToDpc(&g_lock);
    for (NdisDriver **pp = &g_drivers; *pp; pp = &(*pp)->next)
        if (*pp == d) { *pp = d->next; break; }
    nt_KeReleaseSpinLock(&g_lock, old);
    d->magic = 0;
    pool_free(d);
}

/* MiniportSetOptions' call: the PnP handlers (by their header) */
NT_API void nt_NdisMDeregisterWdiMiniportDriver(void *h) { nt_NdisMDeregisterMiniportDriver(h); }

NT_API NDIS_STATUS nt_NdisSetOptionalHandlers(void *h, NDIS_OBJECT_HEADER *chars)
{
    NdisDriver *d = h;
    if (!d || d->magic != M_DRIVER || !chars) return NDIS_STATUS_INVALID_PARAMETER;
    /* (revision 1's size runs through Flags, short of the structure's padding) */
    if (chars->Type == NDIS_OBJECT_TYPE_MINIPORT_PNP_CHARACTERISTICS &&
        chars->Size >= NDIS_OFFSET(NDIS_MINIPORT_PNP_CHARACTERISTICS, Flags)) {
        memset(&d->pnp, 0, sizeof(d->pnp));
        memcpy(&d->pnp, chars, chars->Size < sizeof(d->pnp) ? chars->Size : sizeof(d->pnp));
        d->has_pnp = true;
        return NDIS_STATUS_SUCCESS;
    }
    return NDIS_STATUS_NOT_SUPPORTED;
}

/* The attribute blocks (a union, told apart by its header) */
NT_API NDIS_STATUS nt_NdisMSetMiniportAttributes(void *h, NDIS_OBJECT_HEADER *attr)
{
    NdisAdapter *a = adapter_of(h);
    if (!a || !attr) return NDIS_STATUS_INVALID_PARAMETER;
    if (a->adding) {                                         /* (AddDevice's: its add-device context) */
        a->add_ctx = ((NDIS_MINIPORT_ADD_DEVICE_REGISTRATION_ATTRIBUTES *)attr)->MiniportAddDeviceContext;
        return NDIS_STATUS_SUCCESS;
    }
    switch (attr->Type) {
    case NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES:
        a->ctx = ((NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES *)attr)->MiniportAdapterContext;
        return NDIS_STATUS_SUCCESS;
    case NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES: {
        NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES *g = (void *)attr;
        memcpy(a->mac, g->CurrentMacAddress, 6);
        a->mtu = g->MtuSize;
        a->link = g->MediaConnectState == MediaConnectStateConnected;
        return NDIS_STATUS_SUCCESS;
    }
    default:                                                 /* (offloads, power, NIC switch: none used) */
        return NDIS_STATUS_SUCCESS;
    }
}

NT_API NDIS_STATUS nt_NdisMRemoveMiniport(void *h)
{
    NdisAdapter *a = adapter_of(h);
    if (!a) return NDIS_STATUS_INVALID_PARAMETER;
    kprintf("[NDIS] %s: the miniport reports its adapter failed\n", a->drv->service);
    a->running = false;
    return NDIS_STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * The protocol side a miniport may register (netkvm's failover protocol):
 * accepted, never bound
 * ----------------------------------------------------------------------- */
NT_API NDIS_STATUS nt_NdisRegisterProtocolDriver(void *ctx, NDIS_OBJECT_HEADER *chars, void **out)
{
    if (!chars || !out) return NDIS_STATUS_INVALID_PARAMETER;
    NdisProtocol *p = pool_alloc(sizeof(*p));
    if (!p) return NDIS_STATUS_RESOURCES;
    p->magic = M_PROTOCOL;
    p->ctx = ctx;
    *out = p;
    return NDIS_STATUS_SUCCESS;
}

NT_API void nt_NdisDeregisterProtocolDriver(void *h)
{
    NdisProtocol *p = h;
    if (p && p->magic == M_PROTOCOL) { p->magic = 0; pool_free(p); }
}

NT_API NDIS_STATUS nt_NdisOpenAdapterEx(void *proto, void *ctx, void *params, void *bind_ctx, void **out)
{
    (void)proto; (void)ctx; (void)params; (void)bind_ctx;
    if (out) *out = NULL;
    return NDIS_STATUS_NOT_ACCEPTED;
}

NT_API NDIS_STATUS nt_NdisCloseAdapterEx(void *h) { (void)h; return NDIS_STATUS_SUCCESS; }
NT_API NDIS_STATUS nt_NdisOidRequest(void *h, NDIS_OID_REQUEST *r) { (void)h; (void)r; return NDIS_STATUS_NOT_SUPPORTED; }
NT_API void nt_NdisSendNetBufferLists(void *h, NET_BUFFER_LIST *nbl, UINT32 port, UINT32 flags)
{
    (void)h; (void)nbl; (void)port; (void)flags;
    kprintf("[NDIS] NdisSendNetBufferLists on a binding that does not exist\n");
}
NT_API void nt_NdisReturnNetBufferLists(void *h, NET_BUFFER_LIST *nbl, UINT32 flags) { (void)h; (void)nbl; (void)flags; }

/* Receive polling (NDIS 6.89): not offered */
NT_API NDIS_STATUS nt_NdisRegisterPoll(void *h, void *ctx, void *chars, void **out)
{
    (void)h; (void)ctx; (void)chars;
    if (out) *out = NULL;
    return NDIS_STATUS_NOT_SUPPORTED;
}
NT_API void nt_NdisDeregisterPoll(void *h) { (void)h; }
NT_API void nt_NdisRequestPoll(void *h, void *ctx) { (void)h; (void)ctx; }
NT_API NDIS_STATUS nt_NdisSetPollAffinity(void *h, void *proc) { (void)h; (void)proc; return NDIS_STATUS_NOT_SUPPORTED; }

/* -----------------------------------------------------------------------
 * The adapter's hardware: device properties, configuration space,
 * registers, I/O ports, shared memory
 * ----------------------------------------------------------------------- */
NT_API void nt_NdisMGetDeviceProperty(void *h, NT_DEVICE_OBJECT **pdo, NT_DEVICE_OBJECT **fdo, NT_DEVICE_OBJECT **next,
                                      NT_CM_RESOURCE_LIST **raw, NT_CM_RESOURCE_LIST **xlat)
{
    NdisAdapter *a = adapter_of(h);
    if (!a) return;
    if (pdo) *pdo = a->pdo;
    if (fdo) *fdo = a->fdo;
    if (next) *next = a->lower;
    if (raw) *raw = a->raw;
    if (xlat) *xlat = a->xlat;
}

NT_API UINT32 nt_NdisMGetBusData(void *h, UINT32 space, UINT32 off, void *buf, UINT32 len)
{
    NdisAdapter *a = adapter_of(h);
    if (!a || space != NT_PCI_WHICHSPACE_CONFIG) return 0;
    return NtPnpConfig(a->pdo, false, buf, off, len);
}

NT_API NDIS_STATUS nt_NdisMMapIoSpace(void **va, void *h, INT64 pa, UINT32 len)
{
    (void)h;
    *va = nt_MmMapIoSpace(pa, len, 0);                       /* MmNonCached */
    return *va ? NDIS_STATUS_SUCCESS : NDIS_STATUS_RESOURCES;
}

NT_API void nt_NdisMUnmapIoSpace(void *h, void *va, UINT32 len)
{
    (void)h;
    nt_MmUnmapIoSpace(va, len);
}

/* x86 port I/O: the port numbers are the addresses */
NT_API NDIS_STATUS nt_NdisMRegisterIoPortRange(void **offset, void *h, UINT32 port, UINT32 count)
{
    (void)h; (void)count;
    *offset = (void *)(UINT64)port;
    return NDIS_STATUS_SUCCESS;
}

NT_API void nt_NdisMDeregisterIoPortRange(void *h, UINT32 port, UINT32 count, void *offset)
{
    (void)h; (void)port; (void)count; (void)offset;
}

NT_API void nt_NdisMAllocateSharedMemory(void *h, UINT32 len, UINT8 cached, void **va, INT64 *pa)
{
    (void)h; (void)cached;
    *va = nt_MmAllocateContiguousMemorySpecifyCache(len, 0, -1, 0, 1);
    *pa = *va ? (INT64)nt_MmGetPhysicalAddress(*va) : 0;
}

NT_API void nt_NdisMFreeSharedMemory(void *h, UINT32 len, UINT8 cached, void *va, INT64 pa)
{
    (void)h; (void)len; (void)cached; (void)pa;
    nt_MmFreeContiguousMemory(va);
}

/* -----------------------------------------------------------------------
 * Interrupts
 * ----------------------------------------------------------------------- */
/* Free receive copies */
static UINT32 rx_room(NdisAdapter *a)
{
    return RX_SLOTS - (UINT32)(__atomic_load_n(&a->rx_head, __ATOMIC_ACQUIRE) - __atomic_load_n(&a->rx_tail, __ATOMIC_ACQUIRE));
}

/* The DPC waits for the stack to take frames (net_receive queues it again) */
static void hold_dpc(NdisAdapter *a, void *dpc_ctx)
{
    UINT8 old = nt_KeAcquireSpinLockRaiseToDpc(&a->rx_lock);
    if (rx_room(a)) {                     /* (the stack took frames meanwhile: nothing would wake it) */
        nt_KeInsertQueueDpc(&a->dpc, dpc_ctx, NULL);
    } else {
        a->dpc_held_ctx = dpc_ctx;
        a->dpc_held = true;
    }
    nt_KeReleaseSpinLock(&a->rx_lock, old);
}

static void queue_dpc(NdisAdapter *a, void *dpc_ctx)
{
    if (!rx_room(a)) hold_dpc(a, dpc_ctx);
    else nt_KeInsertQueueDpc(&a->dpc, dpc_ctx, NULL);
}

static NT_API UINT8 isr(void *obj, void *ctx)
{
    (void)obj;
    NdisAdapter *a = ctx;
    UINT8 queue = 0;
    UINT32 target = 0;
    UINT8 mine = ((ISR)a->intr.InterruptHandler)(a->intr_ctx, &queue, &target);
    a->isrs++;
    if (queue) nt_KeInsertQueueDpc(&a->dpc, NULL, NULL);
    return mine;
}

static NT_API void dpc(NT_KDPC *k, void *ctx, void *dpc_ctx, void *unused)
{
    (void)k; (void)unused;
    NdisAdapter *a = ctx;
    /* Windows' stack takes frames inside the DPC; NovaOS's runs in its own
     * thread, so a DPC indicates no more frames than there is room for until
     * that thread has taken them (NDIS' receive throttling), and one that
     * would find no room waits for it (rx_room): the DPC would otherwise
     * run again and again and keep the stack's thread off its processor */
    UINT32 room = rx_room(a);
    if (!room) { hold_dpc(a, dpc_ctx); return; }
    NDIS_RECEIVE_THROTTLE_PARAMETERS t = { room, 0 };
    if (++a->dpcs >= 4096 && a->dpcs >= a->dpc_log) {
        a->dpc_log = a->dpcs * 4;
        kprintf("[NDIS] %s: %llu interrupts, %llu DPCs, %llu frames received (%llu dropped), %llu sent (%llu completed)\n",
                a->drv->service, (unsigned long long)a->isrs, (unsigned long long)a->dpcs,
                (unsigned long long)a->rx_frames, (unsigned long long)a->rx_dropped,
                (unsigned long long)a->tx_frames, (unsigned long long)a->tx_done);
    }
    ((ISR_DPC)a->intr.InterruptDpcHandler)(a->intr_ctx, dpc_ctx, &t, NULL);
    return_pending(a);
    if (t.MoreNblsPending) queue_dpc(a, dpc_ctx);
}

NT_API NDIS_STATUS nt_NdisMRegisterInterruptEx(void *h, void *ctx, NDIS_MINIPORT_INTERRUPT_CHARACTERISTICS *ch,
                                               void **out)
{
    NdisAdapter *a = adapter_of(h);
    if (!a || !ch || !out || !ch->InterruptHandler || !ch->InterruptDpcHandler) return NDIS_STATUS_INVALID_PARAMETER;
    memcpy(&a->intr, ch, sizeof(a->intr));
    a->intr_ctx = ctx;
    nt_KeInitializeDpc(&a->dpc, dpc, a);
    NT_IO_CONNECT_INTERRUPT_PARAMETERS p;
    memset(&p, 0, sizeof(p));
    p.Version = NT_CONNECT_LINE_BASED;                       /* (no MSI for Windows drivers yet) */
    p.LineBased.PhysicalDeviceObject = a->pdo;
    p.LineBased.InterruptObject = &a->intr_obj;
    p.LineBased.ServiceRoutine = isr;
    p.LineBased.ServiceContext = a;
    NT_STATUS st = nt_IoConnectInterruptEx(&p);
    if (!NT_SUCCESS_(st)) {
        kprintf("[NDIS] %s: its interrupt could not be connected (0x%08x)\n", a->drv->service, (UINT32)st);
        return NDIS_STATUS_RESOURCES;
    }
    a->intr_on = true;
    ch->InterruptType = NDIS_CONNECT_LINE_BASED;
    ch->MessageInfoTable = NULL;
    *out = a;
    return NDIS_STATUS_SUCCESS;
}

NT_API void nt_NdisMDeregisterInterruptEx(void *h)
{
    NdisAdapter *a = adapter_of(h);
    if (!a || !a->intr_on) return;
    NT_IO_DISCONNECT_INTERRUPT_PARAMETERS p = { NT_CONNECT_LINE_BASED, a->intr_obj };
    nt_IoDisconnectInterruptEx(&p);
    a->intr_on = false;
    a->intr_obj = NULL;
}

NT_API UINT64 nt_NdisMQueueDpcEx(void *h, UINT32 message, void *target, void *dpc_ctx)
{
    (void)message; (void)target;
    NdisAdapter *a = adapter_of(h);
    if (!a) return 0;
    queue_dpc(a, dpc_ctx);
    return 1;
}

NT_API UINT8 nt_NdisMSynchronizeWithInterruptEx(void *h, UINT32 message, void *fn, void *ctx)
{
    (void)message;
    NdisAdapter *a = adapter_of(h);
    if (!a || !a->intr_obj) return ((SYNC_FN)fn)(ctx);
    return nt_KeSynchronizeExecution(a->intr_obj, (NT_KSYNCHRONIZE_ROUTINE)fn, ctx);
}

/* -----------------------------------------------------------------------
 * Scatter/gather DMA
 * ----------------------------------------------------------------------- */
NT_API NDIS_STATUS nt_NdisMRegisterScatterGatherDma(void *h, NDIS_SG_DMA_DESCRIPTION *d, void **out)
{
    NdisAdapter *a = adapter_of(h);
    if (!a || !d || !out || !d->ProcessSGListHandler) return NDIS_STATUS_INVALID_PARAMETER;
    NT_DEVICE_DESCRIPTION dd;
    memset(&dd, 0, sizeof(dd));
    dd.Version = 3;
    dd.Master = 1;
    dd.ScatterGather = 1;
    dd.Dma64BitAddresses = (d->Flags & NDIS_SG_DMA_64_BIT_ADDRESS) != 0;
    dd.Dma32BitAddresses = 1;
    dd.DmaAddressWidth = dd.Dma64BitAddresses ? 64 : 32;
    dd.InterfaceType = NT_PCIBus;
    dd.MaximumLength = d->MaximumPhysicalMapping;
    UINT32 regs = 0;
    a->dma = nt_IoGetDmaAdapter(a->pdo, &dd, &regs);
    if (!a->dma) return NDIS_STATUS_RESOURCES;
    a->process_sg = d->ProcessSGListHandler;
    UINT32 size = 0;
    a->dma->DmaOperations->CalculateScatterGatherList(a->dma, NULL, (void *)(UINT64)(PAGE_SIZE - 1),
                                                       d->MaximumPhysicalMapping, &size, NULL);
    d->ScatterGatherListSize = size;
    *out = a;
    return NDIS_STATUS_SUCCESS;
}

NT_API void nt_NdisMDeregisterScatterGatherDma(void *h)
{
    NdisAdapter *a = adapter_of(h);
    if (!a || !a->dma) return;
    a->dma->DmaOperations->PutDmaAdapter(a->dma);
    a->dma = NULL;
}

NT_API NDIS_STATUS nt_NdisMAllocateNetBufferSGList(void *h, NET_BUFFER *nb, void *ctx, UINT32 flags, void *buf,
                                                   UINT32 len)
{
    NdisAdapter *a = adapter_of(h);
    if (!a || !a->dma || !nb || !nb->CurrentMdl) return NDIS_STATUS_INVALID_PARAMETER;
    a->sgs++;
    NT_MDL *m = nb->CurrentMdl;
    void *va = (UINT8 *)m->StartVa + m->ByteOffset + nb->CurrentMdlOffset;
    UINT8 write = (flags & NDIS_SG_LIST_WRITE_TO_DEVICE) != 0;
    NT_DMA_OPERATIONS *ops = a->dma->DmaOperations;
    NT_STATUS st = buf ? ops->BuildScatterGatherList(a->dma, a->fdo, m, va, nb->DataLength,
                                                     (NT_DRIVER_LIST_CONTROL)a->process_sg, ctx, write, buf, len)
                       : ops->GetScatterGatherList(a->dma, a->fdo, m, va, nb->DataLength,
                                                   (NT_DRIVER_LIST_CONTROL)a->process_sg, ctx, write);
    return NT_SUCCESS_(st) ? NDIS_STATUS_SUCCESS : NDIS_STATUS_RESOURCES;
}

NT_API void nt_NdisMFreeNetBufferSGList(void *h, NT_SCATTER_GATHER_LIST *list, NET_BUFFER *nb)
{
    (void)nb;
    NdisAdapter *a = adapter_of(h);
    if (a) a->sgs_freed++;
    if (a && a->dma && list) a->dma->DmaOperations->PutScatterGatherList(a->dma, list, 1);
}

/* -----------------------------------------------------------------------
 * NET_BUFFER_LISTs
 * ----------------------------------------------------------------------- */
NT_API void *nt_NdisAllocateNetBufferListPool(void *h, NET_BUFFER_LIST_POOL_PARAMETERS *p)
{
    (void)h;
    if (!p) return NULL;
    NdisPool *pool = pool_alloc(sizeof(*pool));
    if (!pool) return NULL;
    pool->magic = M_POOL;
    pool->p = *p;
    return pool;
}

NT_API void nt_NdisFreeNetBufferListPool(void *h)
{
    NdisPool *pool = h;
    if (pool && pool->magic == M_POOL) { pool->magic = 0; pool_free(pool); }
}

/* Point @nb at @len bytes from @off into the MDL chain @chain */
static void nb_set(NET_BUFFER *nb, NT_MDL *chain, UINT32 off, UINT32 len)
{
    nb->MdlChain = chain;
    nb->DataOffset = off;
    nb->DataLength = len;
    NT_MDL *m = chain;
    while (m && off >= m->ByteCount && m->Next) { off -= m->ByteCount; m = m->Next; }
    nb->CurrentMdl = m;
    nb->CurrentMdlOffset = off;
}

static bool context_alloc(NET_BUFFER_LIST *nbl, UINT16 size, UINT16 backfill)
{
    if (!size && !backfill) return true;
    NET_BUFFER_LIST_CONTEXT *c = pool_alloc(sizeof(*c) + (UINT64)size + backfill);
    if (!c) return false;
    c->Size = (UINT16)(size + backfill);
    c->Offset = backfill;
    nbl->Context = c;
    return true;
}

static void context_free(NET_BUFFER_LIST *nbl)
{
    for (NET_BUFFER_LIST_CONTEXT *c = nbl->Context, *n; c; c = n) { n = c->Next; pool_free(c); }
    nbl->Context = NULL;
}

NT_API NET_BUFFER_LIST *nt_NdisAllocateNetBufferAndNetBufferList(void *h, UINT16 ctx_size, UINT16 backfill,
                                                                  NT_MDL *chain, UINT32 off, UINT64 len)
{
    NdisPool *pool = h;
    if (!pool || pool->magic != M_POOL) return NULL;
    NblBlock *b = pool_alloc(sizeof(*b));
    if (!b) return NULL;
    if (!chain && pool->p.DataSize) {                        /* the pool's own data buffer */
        b->data = pool_alloc(pool->p.DataSize);
        b->mdl = b->data ? nt_IoAllocateMdl(b->data, pool->p.DataSize, 0, 0, NULL) : NULL;
        if (!b->mdl) { pool_free(b->data); pool_free(b); return NULL; }
        nt_MmBuildMdlForNonPagedPool(b->mdl);
        chain = b->mdl;
    }
    NET_BUFFER_LIST *nbl = &b->nbl;
    if (!context_alloc(nbl, ctx_size, backfill)) {
        if (b->mdl) nt_IoFreeMdl(b->mdl);
        pool_free(b->data);
        pool_free(b);
        return NULL;
    }
    nbl->NdisPoolHandle = pool;
    nbl->FirstNetBuffer = &b->nb;
    b->nb.NdisPoolHandle = pool;
    nb_set(&b->nb, chain, off, (UINT32)len);
    return nbl;
}

NT_API void nt_NdisFreeNetBufferList(NET_BUFFER_LIST *nbl)
{
    if (!nbl) return;
    NblBlock *b = BLOCK_OF(nbl);
    context_free(nbl);
    if (b->mdl) nt_IoFreeMdl(b->mdl);
    pool_free(b->data);
    pool_free(b);
}

/* A clone: its own NET_BUFFERs over the original's MDLs */
NT_API void nt_NdisFreeCloneNetBufferList(NET_BUFFER_LIST *nbl, UINT32 flags);

NT_API NET_BUFFER_LIST *nt_NdisAllocateCloneNetBufferList(NET_BUFFER_LIST *orig, void *nbl_pool, void *nb_pool,
                                                          UINT32 flags)
{
    (void)nb_pool; (void)flags;
    if (!orig) return NULL;
    NblBlock *b = pool_alloc(sizeof(*b));
    if (!b) return NULL;
    b->clone = true;
    NET_BUFFER_LIST *nbl = &b->nbl;
    nbl->NdisPoolHandle = nbl_pool;
    nbl->ParentNetBufferList = orig;
    memcpy(nbl->NetBufferListInfo, orig->NetBufferListInfo, sizeof(nbl->NetBufferListInfo));
    NET_BUFFER **tail = &nbl->FirstNetBuffer;
    for (NET_BUFFER *nb = orig->FirstNetBuffer; nb; nb = nb->Next) {
        NET_BUFFER *c = pool_alloc(sizeof(*c));
        if (!c) { nt_NdisFreeCloneNetBufferList(nbl, 0); return NULL; }
        c->MdlChain = nb->MdlChain;
        c->CurrentMdl = nb->CurrentMdl;
        c->CurrentMdlOffset = nb->CurrentMdlOffset;
        c->DataOffset = nb->DataOffset;
        c->DataLength = nb->DataLength;
        c->NdisPoolHandle = nbl_pool;
        *tail = c;
        tail = &c->Next;
    }
    return nbl;
}

NT_API void nt_NdisFreeCloneNetBufferList(NET_BUFFER_LIST *nbl, UINT32 flags)
{
    (void)flags;
    if (!nbl) return;
    NblBlock *b = BLOCK_OF(nbl);
    if (b->clone)
        for (NET_BUFFER *nb = nbl->FirstNetBuffer, *n; nb; nb = n) { n = nb->Next; pool_free(nb); }
    context_free(nbl);
    pool_free(b);
}

/* The miniport's NBLs NovaOS has copied the frames out of go back to it,
 * at DISPATCH_LEVEL, outside its own indication */
static void return_pending(NdisAdapter *a)
{
    if (!a->returns) return;
    UINT8 old = nt_KeAcquireSpinLockRaiseToDpc(&a->rx_lock);
    NET_BUFFER_LIST *list = a->returns;
    a->returns = NULL;
    nt_KeReleaseSpinLock(&a->rx_lock, old);
    if (!list) return;
    old = nt_KfRaiseIrql(DISPATCH_LEVEL);
    ((RETURN)a->drv->ch.ReturnNetBufferListsHandler)(a->ctx, list, NDIS_RETURN_FLAGS_DISPATCH_LEVEL);
    nt_KeLowerIrql(old);
}

NT_API void nt_NdisMIndicateReceiveNetBufferLists(void *h, NET_BUFFER_LIST *list, UINT32 port, UINT32 count,
                                                  UINT32 flags)
{
    (void)port; (void)count;
    NdisAdapter *a = adapter_of(h);
    if (!a || !list) return;
    UINT8 old = nt_KeAcquireSpinLockRaiseToDpc(&a->rx_lock);
    NET_BUFFER_LIST *last = NULL;
    for (NET_BUFFER_LIST *nbl = list; nbl; nbl = nbl->Next) {
        last = nbl;
        for (NET_BUFFER *nb = nbl->FirstNetBuffer; nb; nb = nb->Next) {
            if (!a->running || nb->DataLength > RX_FRAME || a->rx_head - a->rx_tail >= RX_SLOTS) {
                a->rx_dropped++;
                continue;
            }
            UINT32 k = a->rx_head % RX_SLOTS, got = 0;
            UINT8 *dst = a->rx + (UINT64)k * RX_FRAME;
            UINT32 off = nb->CurrentMdlOffset;
            for (NT_MDL *m = nb->CurrentMdl; m && got < nb->DataLength; m = m->Next, off = 0) {
                UINT32 n = m->ByteCount > off ? m->ByteCount - off : 0;
                if (n > nb->DataLength - got) n = nb->DataLength - got;
                memcpy(dst + got, mdl_va(m) + off, n);
                got += n;
            }
            a->rx_len[k] = (UINT16)got;
            a->rx_head++;
            a->rx_frames++;
        }
    }
    if (!(flags & NDIS_RECEIVE_FLAGS_RESOURCES)) {          /* (else the miniport takes them back at once) */
        last->Next = a->returns;
        a->returns = list;
    }
    nt_KeReleaseSpinLock(&a->rx_lock, old);
}

NT_API void nt_NdisMSendNetBufferListsComplete(void *h, NET_BUFFER_LIST *list, UINT32 flags)
{
    (void)flags;
    NdisAdapter *a = adapter_of(h);
    for (NET_BUFFER_LIST *nbl = list, *next; nbl; nbl = next) {
        next = nbl->Next;
        nbl->Next = NULL;
        TxSlot *s = nbl->ProtocolReserved[0];
        if (nbl->Status != NDIS_STATUS_SUCCESS) {
            static int told;
            if (told++ < 4) kprintf("[NDIS] a frame was not sent (0x%08x)\n", (UINT32)nbl->Status);
        }
        if (s) {
            if (a) a->tx_done++;
            __atomic_store_n(&s->busy, 0, __ATOMIC_RELEASE);
        }
    }
}

NT_API void nt_NdisMIndicateStatusEx(void *h, NDIS_STATUS_INDICATION *ind)
{
    NdisAdapter *a = adapter_of(h);
    if (!a || !ind) return;
    if (a->drv->wdi && WDI_IS_INDICATION(ind->StatusCode) && ind->StatusBuffer) {
        WdiIndicate(a, (UINT32)ind->StatusCode, ind->StatusBuffer, ind->StatusBufferSize);
        return;
    }
    if (ind->StatusCode == NDIS_STATUS_LINK_STATE && ind->StatusBuffer && ind->StatusBufferSize >= sizeof(NDIS_LINK_STATE)) {
        NDIS_LINK_STATE *ls = ind->StatusBuffer;
        bool up = ls->MediaConnectState == MediaConnectStateConnected;
        if (up != a->link)
            kprintf("[NDIS] %s: link %s (%llu Mbit/s)\n", a->drv->service, up ? "up" : "down",
                    (unsigned long long)(ls->XmitLinkSpeed / 1000000));
        a->link = up;
    }
}

NT_API void nt_NdisMOidRequestComplete(void *h, NDIS_OID_REQUEST *r, NDIS_STATUS st)
{
    NdisAdapter *a = adapter_of(h);
    if (!a || r != a->oid_waiting) return;
    a->oid_status = st;
    nt_KeSetEvent(&a->oid_done, 0, 0);
}

/* -----------------------------------------------------------------------
 * Memory, MDLs, strings
 * ----------------------------------------------------------------------- */
NT_API void *nt_NdisAllocateMemoryWithTagPriority(void *h, UINT32 len, UINT32 tag, int prio)
{
    (void)h; (void)prio;
    return nt_ExAllocatePoolWithTag(0, len, tag);
}

NT_API void nt_NdisFreeMemoryWithTagPriority(void *h, void *p, UINT32 tag) { (void)h; (void)tag; pool_free(p); }
NT_API void nt_NdisFreeMemory(void *p, UINT32 len, UINT32 flags) { (void)len; (void)flags; pool_free(p); }

NT_API NT_MDL *nt_NdisAllocateMdl(void *h, void *va, UINT32 len)
{
    (void)h;
    NT_MDL *m = nt_IoAllocateMdl(va, len, 0, 0, NULL);
    if (m) nt_MmBuildMdlForNonPagedPool(m);
    return m;
}

NT_API void nt_NdisFreeMdl(NT_MDL *m) { if (m) nt_IoFreeMdl(m); }

NT_API void nt_NdisInitializeString(NT_UNICODE_STRING *u, const char *s)
{
    size_t n = s ? strlen(s) : 0;
    NT_WCHAR *w = nt_ExAllocatePoolWithTag(0, 2 * n + 2, TAG);
    u->Buffer = w;
    u->Length = u->MaximumLength = 0;
    if (!w) return;
    for (size_t i = 0; i <= n; i++) w[i] = i < n ? (UINT8)s[i] : 0;
    u->Length = (unsigned short)(2 * n);
    u->MaximumLength = (unsigned short)(2 * n + 2);
}

/* -----------------------------------------------------------------------
 * Configuration: the driver's service key, or the adapter's driver key
 * (Control\Class\{guid}\NNNN, where its INF's AddReg put its parameters)
 * ----------------------------------------------------------------------- */
static void unicode_ascii(const NT_UNICODE_STRING *u, char *out, int cap)
{
    int n = u && u->Buffer ? u->Length / 2 : 0, k = 0;
    for (int i = 0; i < n && k < cap - 1; i++) out[k++] = u->Buffer[i] < 0x80 ? (char)u->Buffer[i] : '?';
    out[k] = 0;
}

static NdisConfig *config_new(const char *key)
{
    NdisConfig *c = pool_alloc(sizeof(*c));
    if (!c) return NULL;
    c->magic = M_CONFIG;
    strncpy(c->key, key, sizeof(c->key) - 1);
    return c;
}

NT_API NDIS_STATUS nt_NdisOpenConfigurationEx(NDIS_CONFIGURATION_OBJECT *o, void **out)
{
    if (!o || !out) return NDIS_STATUS_INVALID_PARAMETER;
    char key[300];
    NdisAdapter *a = adapter_of(o->NdisHandle);
    NdisDriver *d = o->NdisHandle && ((NdisDriver *)o->NdisHandle)->magic == M_DRIVER ? o->NdisHandle : NULL;
    if (a) {
        const char *inst = NtPnpInstanceOf(a->pdo);
        char ek[200], drvkey[128] = "";
        ksnprintf(ek, sizeof(ek), ENUM_KEY "%s", inst ? inst : "");
        if (inst && um_registry_get_sz(ek, "Driver", drvkey, sizeof(drvkey)) && drvkey[0])
            ksnprintf(key, sizeof(key), CLASS_KEY "%s", drvkey);
        else
            ksnprintf(key, sizeof(key), SERVICES_KEY "%s\\Parameters", a->drv->service);
    } else if (d) {
        ksnprintf(key, sizeof(key), SERVICES_KEY "%s", d->service);
    } else {
        return NDIS_STATUS_INVALID_PARAMETER;
    }
    NdisConfig *c = config_new(key);
    if (!c) return NDIS_STATUS_RESOURCES;
    *out = c;
    return NDIS_STATUS_SUCCESS;
}

NT_API void nt_NdisOpenConfigurationKeyByName(NDIS_STATUS *st, void *h, NT_UNICODE_STRING *name, void **out)
{
    NdisConfig *parent = h;
    *out = NULL;
    if (!parent || parent->magic != M_CONFIG) { *st = NDIS_STATUS_INVALID_PARAMETER; return; }
    char sub[128], key[300];
    unicode_ascii(name, sub, sizeof(sub));
    ksnprintf(key, sizeof(key), "%s\\%s", parent->key, sub);
    if (!um_registry_key_exists(key)) { *st = NDIS_STATUS_FAILURE; return; }
    NdisConfig *c = config_new(key);
    if (!c) { *st = NDIS_STATUS_RESOURCES; return; }
    c->next_sub = parent->subkeys;                           /* (closed with its parent) */
    parent->subkeys = c;
    *out = c;
    *st = NDIS_STATUS_SUCCESS;
}

static bool parse_int(const NT_WCHAR *s, int n, bool hex, UINT32 *out)
{
    UINT32 v = 0;
    int i = 0;
    if (hex && n > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) i = 2;
    if (i >= n) return false;
    for (; i < n && s[i]; i++) {
        NT_WCHAR ch = s[i];
        UINT32 dgt;
        if (ch >= '0' && ch <= '9') dgt = ch - '0';
        else if (hex && ch >= 'a' && ch <= 'f') dgt = ch - 'a' + 10;
        else if (hex && ch >= 'A' && ch <= 'F') dgt = ch - 'A' + 10;
        else return false;
        v = v * (hex ? 16 : 10) + dgt;
    }
    *out = v;
    return true;
}

NT_API void nt_NdisReadConfiguration(NDIS_STATUS *st, NDIS_CONFIGURATION_PARAMETER **out, void *h,
                                     NT_UNICODE_STRING *keyword, int type)
{
    NdisConfig *c = h;
    *out = NULL;
    *st = NDIS_STATUS_FAILURE;
    if (!c || c->magic != M_CONFIG) return;
    char name[128];
    unicode_ascii(keyword, name, sizeof(name));
    UINT8 data[512];
    UINT32 rtype = 0;
    int len = um_registry_get_value(c->key, name, &rtype, data, sizeof(data) - 2);
    if (len < 0) return;
    data[len] = data[len + 1] = 0;
    NdisParam *p = pool_alloc(sizeof(*p) + (UINT64)len + 2);
    if (!p) { *st = NDIS_STATUS_RESOURCES; return; }
    const NT_WCHAR *w = (const NT_WCHAR *)data;
    int wn = len / 2;
    while (wn && !w[wn - 1]) wn--;
    if (rtype == 4 && len >= 4) {                            /* REG_DWORD */
        p->p.ParameterType = type == NdisParameterHexInteger ? NdisParameterHexInteger : NdisParameterInteger;
        memcpy(&p->p.ParameterData.IntegerData, data, 4);
    } else if ((rtype == 1 || rtype == 2) &&
               (type == NdisParameterInteger || type == NdisParameterHexInteger)) {   /* REG_SZ read as a number */
        if (!parse_int(w, wn, type == NdisParameterHexInteger, &p->p.ParameterData.IntegerData)) { pool_free(p); return; }
        p->p.ParameterType = type;
    } else if (rtype == 1 || rtype == 2 || rtype == 7) {
        memcpy(p->text, data, (size_t)len + 2);
        p->p.ParameterType = rtype == 7 ? NdisParameterMultiString : NdisParameterString;
        p->p.ParameterData.StringData.Buffer = p->text;
        p->p.ParameterData.StringData.Length = (unsigned short)(rtype == 7 ? len : 2 * wn);
        p->p.ParameterData.StringData.MaximumLength = (unsigned short)(len + 2);
    } else {
        memcpy(p->text, data, (size_t)len);
        p->p.ParameterType = NdisParameterBinary;
        p->p.ParameterData.BinaryData.Length = (unsigned short)len;
        p->p.ParameterData.BinaryData.Buffer = p->text;
    }
    p->next = c->params;
    c->params = p;
    *out = &p->p;
    *st = NDIS_STATUS_SUCCESS;
}

/* "NetworkAddress": a locally administered MAC address the user set */
NT_API void nt_NdisReadNetworkAddress(NDIS_STATUS *st, void **addr, UINT32 *len, void *h)
{
    NdisConfig *c = h;
    *st = NDIS_STATUS_FAILURE;
    *addr = NULL;
    *len = 0;
    if (!c || c->magic != M_CONFIG) return;
    char s[64];
    if (!um_registry_get_sz(c->key, "NetworkAddress", s, sizeof(s))) return;
    NdisParam *p = pool_alloc(sizeof(*p) + 8);
    if (!p) return;
    UINT8 *mac = (UINT8 *)p->text;
    int n = 0;
    for (const char *q = s; *q && n < 12; q++) {
        int v = *q >= '0' && *q <= '9' ? *q - '0' : *q >= 'a' && *q <= 'f' ? *q - 'a' + 10 : *q >= 'A' && *q <= 'F' ? *q - 'A' + 10 : -1;
        if (v < 0) continue;
        mac[n / 2] = (UINT8)(mac[n / 2] << 4 | v);
        n++;
    }
    if (n != 12) { pool_free(p); return; }
    p->next = c->params;
    c->params = p;
    *addr = mac;
    *len = 6;
    *st = NDIS_STATUS_SUCCESS;
}

static void config_close(NdisConfig *c)
{
    for (NdisConfig *s = c->subkeys, *n; s; s = n) { n = s->next_sub; config_close(s); }
    for (NdisParam *p = c->params, *n; p; p = n) { n = p->next; pool_free(p); }
    c->magic = 0;
    pool_free(c);
}

NT_API void nt_NdisCloseConfiguration(void *h)
{
    NdisConfig *c = h;
    if (c && c->magic == M_CONFIG) config_close(c);
}

/* -----------------------------------------------------------------------
 * Control devices (NdisRegisterDeviceEx): a device of the miniport's
 * own, with its own dispatch routines
 * ----------------------------------------------------------------------- */
NT_API NDIS_STATUS nt_NdisRegisterDeviceEx(void *h, NDIS_DEVICE_OBJECT_ATTRIBUTES *attr, NT_DEVICE_OBJECT **dev,
                                           void **out)
{
    NdisDriver *d = h;
    if (!d || d->magic != M_DRIVER || !attr) return NDIS_STATUS_INVALID_PARAMETER;
    NT_DEVICE_OBJECT *o;
    NT_STATUS st = nt_IoCreateDevice(d->obj, (unsigned)(sizeof(NdisControl) + attr->ExtensionSize), attr->DeviceName,
                                     NT_FILE_DEVICE_UNKNOWN, 0, 0, &o);
    if (!NT_SUCCESS_(st)) return st;
    NdisControl *c = o->DeviceExtension;
    c->magic = M_CONTROL;
    c->drv = d;
    c->dev = o;
    if (attr->MajorFunctions)
        for (int i = 0; i <= NT_IRP_MJ_MAXIMUM_FUNCTION; i++) c->major[i] = attr->MajorFunctions[i];
    if (attr->SymbolicName && attr->SymbolicName->Length) {
        unsigned n = attr->SymbolicName->Length / 2 < 127 ? attr->SymbolicName->Length / 2 : 127;
        memcpy(c->link_buf, attr->SymbolicName->Buffer, n * 2);
        c->link.Buffer = c->link_buf;
        c->link.Length = (unsigned short)(2 * n);
        c->link.MaximumLength = sizeof(c->link_buf);
        if (!NT_SUCCESS_(nt_IoCreateSymbolicLink(&c->link, attr->DeviceName))) c->link.Length = 0;
    }
    o->Flags &= ~NT_DO_DEVICE_INITIALIZING;
    *dev = o;
    *out = c;
    return NDIS_STATUS_SUCCESS;
}

NT_API void nt_NdisDeregisterDeviceEx(void *h)
{
    NdisControl *c = h;
    if (!c || c->magic != M_CONTROL) return;
    if (c->link.Length) nt_IoDeleteSymbolicLink(&c->link);
    c->magic = 0;
    nt_IoDeleteDevice(c->dev);
}

NT_API void *nt_NdisGetDeviceReservedExtension(NT_DEVICE_OBJECT *dev)
{
    NdisControl *c = dev ? dev->DeviceExtension : NULL;
    return c && c->magic == M_CONTROL ? c->reserved : NULL;
}

/* -----------------------------------------------------------------------
 * Synchronization: read/write locks, events, sleeps, work items
 * ----------------------------------------------------------------------- */
#define RW_WRITER 0x80000000u

typedef struct {
    UINT32 magic;
    volatile UINT32 state;              /* readers, or RW_WRITER */
} NdisRwLock;

NT_API void *nt_NdisAllocateRWLock(void *h)
{
    (void)h;
    NdisRwLock *l = pool_alloc(sizeof(*l));
    return l;
}

NT_API void nt_NdisFreeRWLock(void *l) { pool_free(l); }

static UINT8 rw_raise(UINT8 flags)
{
    return flags & NDIS_RWL_AT_DISPATCH_LEVEL ? DISPATCH_LEVEL : nt_KfRaiseIrql(DISPATCH_LEVEL);
}

NT_API void nt_NdisAcquireRWLockRead(NdisRwLock *l, LOCK_STATE_EX *s, UINT8 flags)
{
    s->OldIrql = rw_raise(flags);
    s->Flags = flags;
    for (;;) {                                               /* (readers may nest: no writer preference) */
        UINT32 v = __atomic_load_n(&l->state, __ATOMIC_RELAXED);
        if (!(v & RW_WRITER) && __atomic_compare_exchange_n(&l->state, &v, v + 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            break;
        __builtin_ia32_pause();
    }
    s->LockState = 1;
}

NT_API void nt_NdisAcquireRWLockWrite(NdisRwLock *l, LOCK_STATE_EX *s, UINT8 flags)
{
    s->OldIrql = rw_raise(flags);
    s->Flags = flags;
    for (;;) {
        UINT32 v = 0;
        if (__atomic_compare_exchange_n(&l->state, &v, RW_WRITER, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
        __builtin_ia32_pause();
    }
    s->LockState = 2;
}

NT_API void nt_NdisReleaseRWLock(NdisRwLock *l, LOCK_STATE_EX *s)
{
    if (s->LockState == 2) __atomic_store_n(&l->state, 0, __ATOMIC_RELEASE);
    else __atomic_fetch_sub(&l->state, 1, __ATOMIC_RELEASE);
    s->LockState = 0;
    if (!(s->Flags & NDIS_RWL_AT_DISPATCH_LEVEL)) nt_KeLowerIrql(s->OldIrql);
}

/* NDIS_EVENT: a notification event */
NT_API void nt_NdisInitializeEvent(NT_KEVENT *ev) { nt_KeInitializeEvent(ev, NT_NotificationEvent, 0); }
NT_API void nt_NdisSetEvent(NT_KEVENT *ev) { nt_KeSetEvent(ev, 0, 0); }
NT_API void nt_NdisResetEvent(NT_KEVENT *ev) { nt_KeClearEvent(ev); }

NT_API UINT8 nt_NdisWaitEvent(NT_KEVENT *ev, UINT32 ms)
{
    INT64 t = -(INT64)ms * 10000;
    return nt_KeWaitForSingleObject(ev, 0, 0, 0, ms ? &t : NULL) == NT_STATUS_SUCCESS;
}

NT_API void nt_NdisMSleep(UINT32 us)
{
    if (KeIrql() >= DISPATCH_LEVEL) { nt_KeStallExecutionProcessor(us); return; }
    INT64 t = -(INT64)us * 10;
    nt_KeDelayExecutionThread(0, 0, &t);
}

static NT_API void work_run(NT_DEVICE_OBJECT *dev, void *ctx)
{
    (void)dev;
    NdisWork *w = ctx;
    w->routine(w->ctx, w);
}

NT_API void *nt_NdisAllocateIoWorkItem(void *h)
{
    NdisAdapter *a = adapter_of(h);
    NdisWork *w = pool_alloc(sizeof(*w));
    if (!w) return NULL;
    w->magic = M_WORK;
    w->item = nt_IoAllocateWorkItem(a ? a->fdo : NULL);
    if (!w->item) { pool_free(w); return NULL; }
    return w;
}

NT_API void nt_NdisQueueIoWorkItem(void *h, WORK_ROUTINE routine, void *ctx)
{
    NdisWork *w = h;
    if (!w || w->magic != M_WORK) return;
    w->routine = routine;
    w->ctx = ctx;
    nt_IoQueueWorkItem(w->item, work_run, 1 /* DelayedWorkQueue */, w);
}

NT_API void nt_NdisFreeIoWorkItem(void *h)
{
    NdisWork *w = h;
    if (!w || w->magic != M_WORK) return;
    nt_IoFreeWorkItem(w->item);
    w->magic = 0;
    pool_free(w);
}

/* -----------------------------------------------------------------------
 * Processors and the error log
 * ----------------------------------------------------------------------- */
NT_API UINT32 nt_NdisGroupActiveProcessorCount(UINT16 group) { return nt_KeQueryActiveProcessorCountEx(group); }

NT_API UINT32 nt_NdisProcessorNumberToIndex(UINT32 number)   /* (PROCESSOR_NUMBER by value: Group, Number) */
{
    return nt_KeGetProcessorIndexFromNumber(&number);
}

NT_API void nt_NdisWriteErrorLogEntry(void *h, UINT32 code, UINT32 count, UINT64 v1, UINT64 v2)
{
    NdisAdapter *a = adapter_of(h);
    kprintf("[NDIS] %s: error log entry 0x%08x (%u values%s 0x%llx)\n", a ? a->drv->service : "miniport", code, count,
            count ? ", first" : "", (unsigned long long)(count ? v1 : 0));
    (void)v2;
}

/* -----------------------------------------------------------------------
 * Step 30.9, Intel's AX211 driver: direct OID completion, configuration
 * written back, bus data written, contiguous packet data, the uptime,
 * timer objects, the event log
 * ----------------------------------------------------------------------- */
NT_API UINT8     nt_KeSetTimerEx(NT_KTIMER *t, INT64 due, int period_ms, NT_KDPC *dpc);
NT_API UINT8     nt_KeCancelTimer(NT_KTIMER *t);
NT_API void      nt_KeInitializeTimerEx(NT_KTIMER *t, int type);

NT_API void nt_NdisMDirectOidRequestComplete(void *h, NDIS_OID_REQUEST *r, NDIS_STATUS st)
{
    nt_NdisMOidRequestComplete(h, r, st);
}

NT_API void nt_NdisMResetComplete(void *h, NDIS_STATUS st, UINT8 addressing_reset)
{
    NdisAdapter *a = adapter_of(h);
    (void)addressing_reset;
    if (a) kprintf("[NDIS] %s: reset complete (0x%08x)\n", a->drv->service, (UINT32)st);
}

NT_API UINT32 nt_NdisMSetBusData(void *h, UINT32 space, UINT32 off, void *buf, UINT32 len)
{
    NdisAdapter *a = adapter_of(h);
    if (!a || space != NT_PCI_WHICHSPACE_CONFIG) return 0;
    return NtPnpConfig(a->pdo, true, buf, off, len);
}

/* A value into the key the configuration handle reads (the adapter's
 * driver key), as NdisReadConfiguration would read it back */
NT_API void nt_NdisWriteConfiguration(NDIS_STATUS *st, void *h, NT_UNICODE_STRING *keyword,
                                      NDIS_CONFIGURATION_PARAMETER *v)
{
    NdisConfig *c = h;
    *st = NDIS_STATUS_INVALID_PARAMETER;
    if (!c || c->magic != M_CONFIG || !v) return;
    char name[128];
    unicode_ascii(keyword, name, sizeof(name));
    switch (v->ParameterType) {
    case NdisParameterInteger:
    case NdisParameterHexInteger:
        um_registry_set_dword(c->key, name, v->ParameterData.IntegerData);
        break;
    case NdisParameterString:
    case NdisParameterMultiString: {
        UINT32 n = v->ParameterData.StringData.Length;
        UINT8 *buf = pool_alloc((UINT64)n + 4);
        if (!buf) { *st = NDIS_STATUS_RESOURCES; return; }
        memcpy(buf, v->ParameterData.StringData.Buffer, n);
        um_registry_set_value(c->key, name, v->ParameterType == NdisParameterString ? 1 : 7, buf,
                              n + (v->ParameterType == NdisParameterString ? 2 : 4));
        pool_free(buf);
        break;
    }
    case NdisParameterBinary:
        um_registry_set_bin(c->key, name, v->ParameterData.BinaryData.Buffer, v->ParameterData.BinaryData.Length);
        break;
    default:
        return;
    }
    *st = NDIS_STATUS_SUCCESS;
}

NT_API NDIS_STATUS nt_NdisWriteEventLogEntry(void *source, UINT32 code, UINT32 unique, UINT16 strings, void *string_list,
                                             UINT32 data_size, void *data)
{
    (void)source; (void)unique; (void)strings; (void)string_list; (void)data;
    kprintf("[NDIS] event log entry 0x%08x (%u bytes of data)\n", code, data_size);
    return NDIS_STATUS_SUCCESS;
}

/* @n bytes at the net buffer's data: in place when its current MDL holds
 * them (and @align allows), else copied into @storage, else NULL */
NT_API void *nt_NdisGetDataBuffer(NET_BUFFER *nb, UINT32 n, void *storage, UINT32 align, UINT32 align_off)
{
    if (!nb || n > nb->DataLength || !nb->CurrentMdl) return NULL;
    UINT8 *p = mdl_va(nb->CurrentMdl) + nb->CurrentMdlOffset;
    if (nb->CurrentMdl->ByteCount - nb->CurrentMdlOffset >= n &&
        (align <= 1 || ((UINT64)p % align) == align_off))
        return p;
    if (!storage) return NULL;
    UINT8 *out = storage;
    UINT32 off = nb->CurrentMdlOffset, got = 0;
    for (NT_MDL *m = nb->CurrentMdl; m && got < n; m = m->Next, off = 0) {
        UINT32 take = m->ByteCount - off;
        if (take > n - got) take = n - got;
        memcpy(out + got, mdl_va(m) + off, take);
        got += take;
    }
    return got == n ? storage : NULL;
}

NT_API void nt_NdisGetSystemUpTimeEx(INT64 *ms) { if (ms) *ms = (INT64)(sched_ticks() * 10); }

/* Timer objects: a kernel timer whose DPC calls the miniport's function */
typedef void (NT_API *NDIS_TIMER_FUNCTION)(void *ss1, void *ctx, void *ss2, void *ss3);
typedef struct {
    NDIS_OBJECT_HEADER  Header;
    UINT32              AllocationTag;
    NDIS_TIMER_FUNCTION TimerFunction;
    void               *FunctionContext;
} NDIS_TIMER_CHARACTERISTICS;

#define M_TIMER 0x6D54444Eu
typedef struct {
    UINT32              magic;
    NT_KTIMER           timer;
    NT_KDPC             dpc;
    NDIS_TIMER_FUNCTION fn;
    void               *ctx;
} NdisTimer;

static NT_API void timer_dpc(NT_KDPC *k, void *ctx, void *a1, void *a2)
{
    (void)k; (void)a1; (void)a2;
    NdisTimer *t = ctx;
    if (t->magic == M_TIMER) t->fn(NULL, t->ctx, NULL, NULL);
}

NT_API NDIS_STATUS nt_NdisAllocateTimerObject(void *h, NDIS_TIMER_CHARACTERISTICS *ch, void **out)
{
    (void)h;
    if (!ch || !ch->TimerFunction || !out) return NDIS_STATUS_INVALID_PARAMETER;
    NdisTimer *t = pool_alloc(sizeof(*t));
    if (!t) return NDIS_STATUS_RESOURCES;
    t->magic = M_TIMER;
    t->fn = ch->TimerFunction;
    t->ctx = ch->FunctionContext;
    nt_KeInitializeTimerEx(&t->timer, 0);
    nt_KeInitializeDpc(&t->dpc, timer_dpc, t);
    *out = t;
    return NDIS_STATUS_SUCCESS;
}

NT_API UINT8 nt_NdisSetTimerObject(NdisTimer *t, INT64 due, UINT32 period_ms, void *ctx)
{
    if (!t || t->magic != M_TIMER) return 0;
    if (ctx) t->ctx = ctx;
    return nt_KeSetTimerEx(&t->timer, due, (int)period_ms, &t->dpc);
}

NT_API UINT8 nt_NdisSetCoalescableTimerObject(NdisTimer *t, INT64 due, UINT32 period_ms, void *ctx, UINT32 tolerance)
{
    (void)tolerance;
    return nt_NdisSetTimerObject(t, due, period_ms, ctx);
}

NT_API UINT8 nt_NdisCancelTimerObject(NdisTimer *t)
{
    return t && t->magic == M_TIMER ? nt_KeCancelTimer(&t->timer) : 0;
}

NT_API void nt_NdisFreeTimerObject(NdisTimer *t)
{
    if (!t || t->magic != M_TIMER) return;
    nt_KeCancelTimer(&t->timer);
    t->magic = 0;
    pool_free(t);
}
