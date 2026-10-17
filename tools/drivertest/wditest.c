/*
 * wditest.sys — a WDI Wi-Fi miniport for QEMU's edu device, NovaOS's test
 * of the WDI command layer (roadmap step 28.1, kernel/nt/wdi.c)
 *
 * QEMU has no Wi-Fi card, and Intel's Netwtw14.sys stops at its hardware
 * probe on a stand-in (the devices suite's ax211 boot), so this driver is
 * the Wi-Fi card: a WDI lower edge as Intel's is, registered with
 * NdisMRegisterWdiMiniportDriver, whose radio is a table of three access
 * points.  It answers the WDI messages NovaOS's upper edge sends as an IHV
 * driver must: OID requests of type NdisRequestMethod whose buffer is a
 * WDI_MESSAGE_HEADER and TLVs (kernel/nt/wdi.h), the answer written into
 * the same buffer; a task (create port, scan, delete port) is finished by
 * an NDIS_STATUS_WDI_INDICATION_*_COMPLETE status indication with the
 * task's TransactionId, from a timer, after the OID request; a scan pends
 * its OID request, indicates the access points it "hears" (WDI_TLV_BSS_ENTRY:
 * BSSID, signal, channel and the beacon's frame body), completes the
 * request with NdisMOidRequestComplete and then indicates the scan's end.
 * Each check it makes of what it is sent goes to the debugger (DbgPrint),
 * where the wifi self-test reads it.
 *
 * Built by tools/drivertest/mksys.py; the wifi self-test installs it with
 * "pnputil /add-driver wditest.inf /install" and scans with
 * "netsh wlan show networks mode=bssid".
 */
#include "../../kernel/nt/ndis.h"
#include "../../kernel/nt/wdi.h"

#define IMPORT __declspec(dllimport) NT_API
IMPORT unsigned int DbgPrint(const char *fmt, ...);
IMPORT NDIS_STATUS NdisMRegisterWdiMiniportDriver(NT_DRIVER_OBJECT *obj, NT_UNICODE_STRING *reg, void *ctx,
                                                  NDIS_MINIPORT_DRIVER_CHARACTERISTICS *ch,
                                                  NDIS_MINIPORT_DRIVER_WDI_CHARACTERISTICS *wdi, void **out);
IMPORT void        NdisMDeregisterWdiMiniportDriver(void *h);
IMPORT void       *NdisAllocateMemoryWithTagPriority(void *h, unsigned int len, unsigned int tag, int prio);
IMPORT void        NdisFreeMemoryWithTagPriority(void *h, void *p, unsigned int tag);
IMPORT void        NdisMIndicateStatusEx(void *h, NDIS_STATUS_INDICATION *ind);
IMPORT void        NdisMOidRequestComplete(void *h, NDIS_OID_REQUEST *r, NDIS_STATUS st);

typedef void (NT_API *TIMER_FN)(void *ss1, void *ctx, void *ss2, void *ss3);
typedef struct {
    NDIS_OBJECT_HEADER Header;
    unsigned int       AllocationTag;
    TIMER_FN           TimerFunction;
    void              *FunctionContext;
} TIMER_CHARACTERISTICS;
IMPORT NDIS_STATUS NdisAllocateTimerObject(void *h, TIMER_CHARACTERISTICS *ch, void **out);
IMPORT unsigned char NdisSetTimerObject(void *t, long long due, unsigned int period_ms, void *ctx);
IMPORT unsigned char NdisCancelTimerObject(void *t);
IMPORT void        NdisFreeTimerObject(void *t);

typedef void (NT_API *COMPLETE_FN)(void *adapter_handle, NDIS_STATUS st);

#define TAG       0x54494457u       /* 'WDIT' */
#define WDI_VER   ((1 << 16) | (1 << 8) | 8)
#define PORT      1
#define NDIS_OBJECT_TYPE_TIMER_CHARACTERISTICS 0x97

static void *g_driver;

/* ---- the radio: three access points ---- */
static const unsigned char MAC[6] = { 0x02, 0x4e, 0x6f, 0x76, 0x61, 0x00 };

typedef struct {
    unsigned char  bssid[6];
    const char    *ssid;
    int            rssi;
    unsigned int   quality, channel, band;
    unsigned short capability;
    const unsigned char *ies;       /* after the SSID element */
    unsigned int   ies_len;
} Ap;

/* Supported rates, DS parameter set (channel 6), RSN (CCMP, PSK), HT capabilities */
static const unsigned char IES_WPA2[] = {
    1, 8, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24,
    3, 1, 6,
    48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 2, 0x0c, 0x00,
    45, 26, 0x6f, 0x01, 0x17, 0xff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
/* Rates, RSN (CCMP, SAE, management frame protection required), HT, VHT, HE */
static const unsigned char IES_WPA3[] = {
    1, 8, 0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c,
    48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 8, 0xc0, 0x00,
    45, 26, 0xef, 0x09, 0x17, 0xff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    191, 12, 0x91, 0x59, 0x82, 0x0f, 0xea, 0xff, 0, 0, 0xea, 0xff, 0, 0,
    255, 7, 35, 0x05, 0x00, 0x08, 0x12, 0x00, 0x10,
};
/* Rates, DS parameter set (channel 11), HT: an open network */
static const unsigned char IES_OPEN[] = {
    1, 8, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24,
    3, 1, 11,
    45, 26, 0x2c, 0x01, 0x17, 0xff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static const Ap APS[] = {
    { { 0x02, 0x4e, 0x6f, 0x76, 0x61, 0x01 }, "NovaNet",    -45, 90,  6, WDI_BAND_ID_2400, 0x0411,
      IES_WPA2, sizeof(IES_WPA2) },
    { { 0x02, 0x4e, 0x6f, 0x76, 0x61, 0x02 }, "NovaNet-5G", -60, 72, 36, WDI_BAND_ID_5000, 0x0011,
      IES_WPA3, sizeof(IES_WPA3) },
    { { 0x02, 0x4e, 0x6f, 0x76, 0x61, 0x03 }, "NovaGuest",  -78, 35, 11, WDI_BAND_ID_2400, 0x0401,
      IES_OPEN, sizeof(IES_OPEN) },
};
#define NAPS (int)(sizeof(APS) / sizeof(APS[0]))

/* ---- the adapter ---- */
typedef struct {
    void             *handle;       /* NDIS's adapter handle */
    COMPLETE_FN       open_done, close_done;
    void             *timer;
    /* the task the timer finishes */
    unsigned int      task, tid;
    unsigned short    port;
    NDIS_OID_REQUEST *pending;      /* a scan's OID request, completed by the timer */
    int               port_up;
    unsigned char     ind[2048];
} Adapter;

static void *zero(void *p, unsigned long long n)
{
    volatile unsigned char *b = p;
    while (n--) *b++ = 0;
    return p;
}

static void copy(void *d, const void *s, unsigned long long n)
{
    unsigned char *a = d;
    const unsigned char *b = s;
    while (n--) *a++ = *b++;
}

static unsigned int put(unsigned char *buf, unsigned int off, unsigned short type, const void *v, unsigned int n)
{
    WDI_TLV_HEADER h = { type, (unsigned short)n };
    copy(buf + off, &h, sizeof(h));
    if (n) copy(buf + off + sizeof(h), v, n);
    return off + sizeof(h) + n;
}

/* A TLV's value at the top level of @buf, or NULL */
static const unsigned char *find(const unsigned char *buf, unsigned int len, unsigned short type, unsigned int *n)
{
    for (unsigned int off = 0; off + sizeof(WDI_TLV_HEADER) <= len;) {
        const WDI_TLV_HEADER *h = (const WDI_TLV_HEADER *)(buf + off);
        if (off + sizeof(*h) + h->Length > len) return 0;
        if (h->Type == type) { *n = h->Length; return buf + off + sizeof(*h); }
        off += sizeof(*h) + h->Length;
    }
    return 0;
}

static void indicate(Adapter *a, unsigned int id, unsigned int tid, unsigned int len)
{
    WDI_MESSAGE_HEADER *h = (WDI_MESSAGE_HEADER *)a->ind;
    h->PortId = a->port;
    h->Reserved = 0;
    h->Status = NDIS_STATUS_SUCCESS;
    h->TransactionId = tid;
    h->IhvSpecificId = 0;
    NDIS_STATUS_INDICATION ind;
    zero(&ind, sizeof(ind));
    ind.Header.Type = NDIS_OBJECT_TYPE_STATUS_INDICATION;
    ind.Header.Revision = 1;
    ind.Header.Size = sizeof(ind);
    ind.SourceHandle = a->handle;
    ind.StatusCode = (NDIS_STATUS)WDI_INDICATION(id);
    ind.StatusBuffer = a->ind;
    ind.StatusBufferSize = sizeof(*h) + len;
    NdisMIndicateStatusEx(a->handle, &ind);
}

/* One WDI_TLV_BSS_ENTRY for @ap at @off in @buf */
static unsigned int bss_entry(unsigned char *buf, unsigned int off, const Ap *ap)
{
    unsigned char e[400], frame[300];
    unsigned int n = 0, f = 0, sl = 0;
    while (ap->ssid[sl]) sl++;
    zero(frame, 12);                                        /* timestamp */
    frame[8] = 100;                                         /* beacon interval (TU) */
    frame[10] = (unsigned char)ap->capability;
    frame[11] = (unsigned char)(ap->capability >> 8);
    f = 12;
    frame[f++] = 0;
    frame[f++] = (unsigned char)sl;
    copy(frame + f, ap->ssid, sl);
    f += sl;
    copy(frame + f, ap->ies, ap->ies_len);
    f += ap->ies_len;
    WDI_SIGNAL_INFO si = { ap->rssi, ap->quality };
    WDI_BSS_ENTRY_CHANNEL_INFO ci = { ap->channel, ap->band };
    n = put(e, n, WDI_TLV_BSSID, ap->bssid, 6);
    n = put(e, n, WDI_TLV_BEACON_FRAME, frame, f);
    n = put(e, n, WDI_TLV_BSS_ENTRY_SIGNAL_INFO, &si, sizeof(si));
    n = put(e, n, WDI_TLV_BSS_ENTRY_CHANNEL_INFO, &ci, sizeof(ci));
    return put(buf, off, WDI_TLV_BSS_ENTRY, e, n);
}

/* The timer: finish the task the last OID request started */
static NT_API void task_timer(void *ss1, void *ctx, void *ss2, void *ss3)
{
    (void)ss1; (void)ss2; (void)ss3;
    Adapter *a = ctx;
    unsigned char *t = a->ind + sizeof(WDI_MESSAGE_HEADER);
    unsigned int n = 0, task = a->task;
    a->task = 0;                    /* (the next request may come as soon as the task is done) */
    switch (task) {
    case WDI_TASK_CREATE_PORT: {
        WDI_PORT_ATTRIBUTES pa;
        copy(pa.MacAddress, MAC, 6);
        pa.PortNumber = PORT;
        n = put(t, 0, WDI_TLV_PORT_ATTRIBUTES, &pa, sizeof(pa));
        a->port = WDI_PORT_ID_ADAPTER;
        a->port_up = 1;
        indicate(a, WDI_INDICATION_CREATE_PORT_COMPLETE, a->tid, n);
        DbgPrint("wditest: station port %u created\n", PORT);
        break;
    }
    case WDI_TASK_DELETE_PORT:
        a->port = WDI_PORT_ID_ADAPTER;
        a->port_up = 0;
        indicate(a, WDI_INDICATION_DELETE_PORT_COMPLETE, a->tid, 0);
        DbgPrint("wditest: station port %u deleted\n", PORT);
        break;
    case WDI_TASK_SCAN: {
        /* what the radio heard: two entries unsolicited as they come in, the
         * last with the task's id; then the request completes and the scan ends */
        a->port = PORT;
        for (int i = 0; i < 2; i++) n = bss_entry(t, n, &APS[i]);
        indicate(a, WDI_INDICATION_BSS_ENTRY_LIST, WDI_TRANSACTION_ID_UNSOLICIT, n);
        n = bss_entry(t, 0, &APS[2]);
        indicate(a, WDI_INDICATION_BSS_ENTRY_LIST, a->tid, n);
        NDIS_OID_REQUEST *r = a->pending;
        a->pending = 0;
        if (r) {
            r->DATA.METHOD_INFORMATION.BytesWritten = sizeof(WDI_MESSAGE_HEADER);
            NdisMOidRequestComplete(a->handle, r, NDIS_STATUS_SUCCESS);
        }
        indicate(a, WDI_INDICATION_SCAN_COMPLETE, a->tid, 0);
        DbgPrint("wditest: scan complete: %d BSS entries indicated\n", NAPS);
        break;
    }
    }
}

static void start_task(Adapter *a, unsigned int task, unsigned int tid, int ms)
{
    a->task = task;
    a->tid = tid;
    NdisSetTimerObject(a->timer, -(long long)ms * 10000, 0, a);
}

/* ---- the WDI messages ---- */
static NDIS_STATUS capabilities(unsigned char *t, unsigned int room, unsigned int *len)
{
    unsigned char ia[200], caps[64], bi[32];
    zero(caps, sizeof(caps));
    WDI_INTERFACE_CAPABILITIES *ic = (WDI_INTERFACE_CAPABILITIES *)caps;
    ic->MTUSize = 1500;
    ic->MaxMultiCastListSize = 32;
    copy(ic->Address, MAC, 6);
    ic->MaxTxRate = ic->MaxRxRate = 1201000;
    ic->HardwareRadioState = ic->SoftwareRadioState = 1;
    static const char fw[] = "wditest 1.0";
    unsigned int n = put(ia, 0, WDI_TLV_INTERFACE_CAPABILITIES, caps, 40);
    n = put(ia, n, WDI_TLV_FIRMWARE_VERSION, fw, sizeof(fw));
    unsigned int out = put(t, 0, WDI_TLV_INTERFACE_ATTRIBUTES, ia, n);
    out = put(t, out, WDI_TLV_STATION_ATTRIBUTES, 0, 0);
    static const unsigned int bands[] = { WDI_BAND_ID_2400, WDI_BAND_ID_5000 };
    for (int i = 0; i < 2; i++) {
        WDI_BAND_CAPABILITIES bc = { bands[i], 1 };
        unsigned int m = put(bi, 0, WDI_TLV_BAND_CAPABILITIES, &bc, sizeof(bc));
        out = put(t, out, WDI_TLV_BAND_INFO, bi, m);
    }
    if (out > room) return NDIS_STATUS_BUFFER_TOO_SHORT;
    *len = out;
    DbgPrint("wditest: adapter capabilities answered (%u bytes of TLVs)\n", out);
    return NDIS_STATUS_SUCCESS;
}

static NT_API NDIS_STATUS oid_request(void *ctx, NDIS_OID_REQUEST *r)
{
    Adapter *a = ctx;
    if (r->RequestType != NdisRequestMethod) return NDIS_STATUS_NOT_SUPPORTED;
    unsigned int oid = r->DATA.METHOD_INFORMATION.Oid;
    unsigned char *buf = r->DATA.METHOD_INFORMATION.InformationBuffer;
    unsigned int in = r->DATA.METHOD_INFORMATION.InputBufferLength;
    unsigned int room = r->DATA.METHOD_INFORMATION.OutputBufferLength;
    if ((oid & 0xFFFF0000u) != WDI_OID(0) || in < sizeof(WDI_MESSAGE_HEADER) || room < sizeof(WDI_MESSAGE_HEADER)) {
        DbgPrint("wditest: OID 0x%08x is not a WDI message\n", oid);
        return NDIS_STATUS_INVALID_OID;
    }
    WDI_MESSAGE_HEADER *h = (WDI_MESSAGE_HEADER *)buf;
    unsigned char *t = buf + sizeof(*h);
    unsigned int tlen = in - sizeof(*h), out = 0, n;
    unsigned int msg = oid & 0xFFFF;
    NDIS_STATUS st = NDIS_STATUS_SUCCESS;
    if (a->task) return NDIS_STATUS_FAILURE;                /* (one task at a time) */
    switch (msg) {
    case WDI_GET_ADAPTER_CAPABILITIES:
        if (h->PortId != WDI_PORT_ID_ADAPTER) DbgPrint("wditest: capabilities asked of port %u\n", h->PortId);
        st = capabilities(t, room - sizeof(*h), &out);
        break;
    case WDI_TASK_CREATE_PORT: {
        WDI_CREATE_PORT_PARAMETERS p;
        const unsigned char *v = find(t, tlen, WDI_TLV_CREATE_PORT_PARAMETERS, &n);
        if (!v || n < sizeof(p) || a->port_up) { st = NDIS_STATUS_INVALID_PARAMETER; break; }
        copy(&p, v, sizeof(p));
        DbgPrint("wditest: create port task %u: modes 0x%x, NDIS port %u\n", h->TransactionId, p.OpModeMask,
                 p.NdisPortNumber);
        start_task(a, msg, h->TransactionId, 10);
        break;
    }
    case WDI_TASK_DELETE_PORT: {
        WDI_DELETE_PORT_PARAMETERS p;
        const unsigned char *v = find(t, tlen, WDI_TLV_DELETE_PORT_PARAMETERS, &n);
        if (!v || n < sizeof(p)) { st = NDIS_STATUS_INVALID_PARAMETER; break; }
        copy(&p, v, sizeof(p));
        if (p.PortNumber != PORT || !a->port_up) { st = NDIS_STATUS_INVALID_PARAMETER; break; }
        start_task(a, msg, h->TransactionId, 10);
        break;
    }
    case WDI_TASK_SCAN: {
        WDI_SCAN_MODE mode;
        WDI_SCAN_DWELL_TIME dw;
        const unsigned char *b = find(t, tlen, WDI_TLV_BSSID, &n);
        int wild = b && n == 6 && b[0] == 0xff && b[1] == 0xff && b[2] == 0xff && b[3] == 0xff && b[4] == 0xff &&
                   b[5] == 0xff;
        const unsigned char *s = find(t, tlen, WDI_TLV_SSID, &n);
        unsigned int ssid_len = s ? n : 0;
        const unsigned char *m = find(t, tlen, WDI_TLV_SCAN_MODE, &n);
        if (!m || n < sizeof(mode)) m = 0; else copy(&mode, m, sizeof(mode));
        const unsigned char *d = find(t, tlen, WDI_TLV_SCAN_DWELL_TIME, &n);
        if (!d || n < sizeof(dw)) d = 0; else copy(&dw, d, sizeof(dw));
        if (h->PortId != PORT || !a->port_up || !b || !s || !m || !d) {
            DbgPrint("wditest: scan task refused: port %u, BSSID %s, SSID %s, mode %s, dwell %s\n", h->PortId,
                     b ? "yes" : "no", s ? "yes" : "no", m ? "yes" : "no", d ? "yes" : "no");
            st = NDIS_STATUS_INVALID_PARAMETER;
            break;
        }
        DbgPrint("wditest: scan task %u on port %u: %s BSSID, %s, type %u, dwell %u/%u ms, at most %u ms\n",
                 h->TransactionId, h->PortId, wild ? "wildcard" : "one", ssid_len ? "one SSID" : "any SSID",
                 mode.AllowedScanType, dw.ActiveChannelDwellTime, dw.PassiveChannelDwellTime, dw.MaximumScanTime);
        h->Status = NDIS_STATUS_SUCCESS;
        a->pending = r;
        start_task(a, msg, h->TransactionId, 200);
        return NDIS_STATUS_PENDING;
    }
    default:
        DbgPrint("wditest: WDI message %u not handled\n", msg);
        st = NDIS_STATUS_NOT_SUPPORTED;
    }
    h->Status = st;
    r->DATA.METHOD_INFORMATION.BytesWritten = sizeof(*h) + out;
    return st;
}

/* ---- the WDI lower edge ---- */
static NT_API NDIS_STATUS allocate_adapter(void *handle, void *driver_ctx, NDIS_MINIPORT_INIT_PARAMETERS *init,
                                           NDIS_WDI_INIT_PARAMETERS *wdi, NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES *ra)
{
    (void)driver_ctx; (void)init;
    Adapter *a = NdisAllocateMemoryWithTagPriority(handle, sizeof(*a), TAG, 32);
    if (!a) return NDIS_STATUS_RESOURCES;
    zero(a, sizeof(*a));
    a->handle = handle;
    a->port = WDI_PORT_ID_ADAPTER;
    a->open_done = (COMPLETE_FN)wdi->OpenAdapterCompleteHandler;
    a->close_done = (COMPLETE_FN)wdi->CloseAdapterCompleteHandler;
    TIMER_CHARACTERISTICS tc = { { NDIS_OBJECT_TYPE_TIMER_CHARACTERISTICS, 1, sizeof(tc) }, TAG, task_timer, a };
    if (NdisAllocateTimerObject(handle, &tc, &a->timer) != NDIS_STATUS_SUCCESS) {
        NdisFreeMemoryWithTagPriority(handle, a, TAG);
        return NDIS_STATUS_RESOURCES;
    }
    ra->MiniportAdapterContext = a;
    DbgPrint("wditest: adapter allocated (WDI version 0x%x)\n", wdi->WdiVersion);
    return NDIS_STATUS_SUCCESS;
}

static NT_API void free_adapter(void *ctx)
{
    Adapter *a = ctx;
    NdisFreeTimerObject(a->timer);
    NdisFreeMemoryWithTagPriority(a->handle, a, TAG);
    DbgPrint("wditest: adapter freed\n");
}

static NT_API NDIS_STATUS open_adapter(void *ctx, NDIS_MINIPORT_INIT_PARAMETERS *init)
{
    (void)init;
    Adapter *a = ctx;
    DbgPrint("wditest: adapter open: radio on, %d access points in range\n", NAPS);
    a->open_done(a->handle, NDIS_STATUS_SUCCESS);
    return NDIS_STATUS_SUCCESS;
}

static NT_API NDIS_STATUS close_adapter(void *ctx)
{
    Adapter *a = ctx;
    NdisCancelTimerObject(a->timer);
    DbgPrint("wditest: adapter closed%s\n", a->port_up ? " (its port still up)" : "");
    a->close_done(a->handle, NDIS_STATUS_SUCCESS);
    return NDIS_STATUS_SUCCESS;
}

static NT_API void unload(NT_DRIVER_OBJECT *obj)
{
    (void)obj;
    NdisMDeregisterWdiMiniportDriver(g_driver);
    DbgPrint("wditest: unloaded\n");
}

NT_API NT_STATUS DriverEntry(NT_DRIVER_OBJECT *obj, NT_UNICODE_STRING *reg)
{
    NDIS_MINIPORT_DRIVER_CHARACTERISTICS ch;
    NDIS_MINIPORT_DRIVER_WDI_CHARACTERISTICS wdi;
    zero(&ch, sizeof(ch));
    zero(&wdi, sizeof(wdi));
    ch.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_DRIVER_CHARACTERISTICS;
    ch.Header.Revision = 2;
    ch.Header.Size = sizeof(ch);
    ch.MajorNdisVersion = 6;
    ch.MinorNdisVersion = 50;
    ch.MajorDriverVersion = 1;
    ch.UnloadHandler = (void *)unload;
    ch.OidRequestHandler = (void *)oid_request;
    wdi.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_WDI_CHARACTERISTICS;
    wdi.Header.Revision = 1;
    wdi.Header.Size = sizeof(wdi);
    wdi.WdiVersion = WDI_VER;
    wdi.AllocateAdapterHandler = (void *)allocate_adapter;
    wdi.FreeAdapterHandler = (void *)free_adapter;
    wdi.OpenAdapterHandler = (void *)open_adapter;
    wdi.CloseAdapterHandler = (void *)close_adapter;
    NDIS_STATUS st = NdisMRegisterWdiMiniportDriver(obj, reg, 0, &ch, &wdi, &g_driver);
    DbgPrint("wditest: DriverEntry %08x\n", (unsigned int)st);
    return st == NDIS_STATUS_SUCCESS ? 0 : (NT_STATUS)0xC0000001;
}
