/*
 * wdi.c — the WDI command layer: NovaOS's upper edge for a Wi-Fi miniport
 * (roadmap step 28.1)
 *
 * On Windows a Wi-Fi driver such as Intel's Netwtw14.sys is a WDI "lower
 * edge": it does not talk to NDIS's protocols itself, Microsoft's
 * wdiwifi.sys is the NDIS miniport for it and drives it with WDI messages.
 * ndis.c is the part of that upper edge that allocates and opens the
 * adapter (MiniportWdiAllocateAdapter, MiniportWdiOpenAdapter); this file is
 * the rest of what NovaOS has of it so far, the command layer:
 *
 *   - a command is an NDIS method OID (OID_WDI_*) whose buffer holds a
 *     WDI_MESSAGE_HEADER and TLVs (wdi.h); ndis.c hands it to the
 *     miniport's MiniportOidRequest and waits for it, also when it pends;
 *   - a task (OID_WDI_TASK_*) is accepted by that request and finished by
 *     a status indication, NDIS_STATUS_WDI_INDICATION_*_COMPLETE, whose
 *     header carries the task's TransactionId; a command here waits for
 *     both;
 *   - other indications come when the driver has something to say: a
 *     scan's BSS entries (NDIS_STATUS_WDI_INDICATION_BSS_ENTRY_LIST), kept
 *     here as the adapter's list of access points.
 *
 * After the open, WdiAttach asks the adapter what it is
 * (OID_WDI_GET_ADAPTER_CAPABILITIES: its MAC address, firmware, radio state
 * and bands) and creates its station port (OID_WDI_TASK_CREATE_PORT); a
 * scan (WlanScan, from netsh wlan show networks) is OID_WDI_TASK_SCAN on
 * that port.  Not yet here (docs/drivers.md): the data path (TAL TX/RX),
 * the adapter configuration, connecting (OID_WDI_TASK_CONNECT and the
 * WPA2/WPA3 handshake), wlanapi.dll and Settings' Wi-Fi page.
 */
#include "ntapi.h"
#include "ndis.h"
#include "wdi.h"
#include "wlan.h"
#include "../ke/printf.h"
#include "../ke/scheduler.h"
#include "../lib/string.h"
#include "../mm/vmm.h"

#define WDI_MAX_ADAPTERS  4
#define CMD_BUF           8192      /* (a command's buffer: its answer comes back in it) */
#define TASK_WAIT_S       30
#define SCAN_MAX_MS       4000

struct WdiHost {
    bool           used;
    void          *adapter;         /* ndis.c's NdisAdapter; NULL once detached */
    WdiRequestFn   request;
    WlanInfo       info;
    UINT32         next_tid;
    UINT16         port_id;         /* the station port's WDI port number */
    NT_KEVENT      cmd_lock;        /* (one command at a time: a synchronization event) */
    /* the task being waited for */
    volatile UINT32 task_tid;
    UINT32         task_done_id;    /* the WDI_INDICATION_* that finishes it */
    NT_KEVENT      task_done;
    NDIS_STATUS    task_status;
    WDI_PORT_ATTRIBUTES port_attr;  /* (from WDI_INDICATION_CREATE_PORT_COMPLETE) */
    /* the access points the driver has indicated */
    NT_KSPIN_LOCK  bss_lock;
    WlanBss        bss[WLAN_MAX_BSS];
    int            nbss;
};

/* (hosts are never freed, only reused: netsh can hold one while its
 * device goes away, and finds it detached when it gets the command lock) */
static NT_KSPIN_LOCK g_lock;
static WdiHost       g_hosts[WDI_MAX_ADAPTERS];

/* -----------------------------------------------------------------------
 * TLVs
 * ----------------------------------------------------------------------- */

/* The value of the first @type TLV at the top level of @buf, or NULL */
static const UINT8 *tlv_find(const UINT8 *buf, UINT32 len, UINT16 type, UINT32 *vlen)
{
    UINT32 off = 0;
    while (off + sizeof(WDI_TLV_HEADER) <= len) {
        const WDI_TLV_HEADER *h = (const WDI_TLV_HEADER *)(buf + off);
        UINT32 n = h->Length;
        if (off + sizeof(*h) + n > len) return NULL;
        if (h->Type == type) {
            *vlen = n;
            return buf + off + sizeof(*h);
        }
        off += sizeof(*h) + n;
    }
    return NULL;
}

/* Copy a TLV's value into @dst when it is at least @need bytes long */
static bool tlv_get(const UINT8 *buf, UINT32 len, UINT16 type, void *dst, UINT32 need)
{
    UINT32 n;
    const UINT8 *v = tlv_find(buf, len, type, &n);
    if (!v || n < need) return false;
    memcpy(dst, v, need);
    return true;
}

static UINT32 tlv_put(UINT8 *buf, UINT32 off, UINT16 type, const void *val, UINT16 n)
{
    WDI_TLV_HEADER h = { type, n };
    memcpy(buf + off, &h, sizeof(h));
    if (n) memcpy(buf + off + sizeof(h), val, n);
    return off + sizeof(h) + n;
}

/* -----------------------------------------------------------------------
 * Commands
 * ----------------------------------------------------------------------- */

static void lock(WdiHost *w)   { nt_KeWaitForSingleObject(&w->cmd_lock, 0, 0, 0, NULL); }
static void unlock(WdiHost *w) { nt_KeSetEvent(&w->cmd_lock, 0, 0); }

/* Send WDI command @msg (OID_WDI_*) with the @tlen bytes of TLVs in
 * @buf + sizeof(WDI_MESSAGE_HEADER) to port @port.  A task (@done_id, its
 * completion indication, non-zero) is waited for until that indication
 * comes.  The answer's TLVs are left in @buf, *@rlen bytes of them; its
 * status is returned.  The caller holds the command lock. */
static NDIS_STATUS command(WdiHost *w, UINT32 msg, UINT16 port, UINT8 *buf, UINT32 tlen, UINT32 done_id,
                           UINT32 *rlen, UINT32 wait_s)
{
    WDI_MESSAGE_HEADER *h = (WDI_MESSAGE_HEADER *)buf;
    memset(h, 0, sizeof(*h));
    h->PortId = port;
    h->TransactionId = ++w->next_tid ? w->next_tid : ++w->next_tid;
    if (done_id) {
        nt_KeClearEvent(&w->task_done);
        w->task_done_id = done_id;
        w->task_status = NDIS_STATUS_FAILURE;
        __atomic_store_n(&w->task_tid, h->TransactionId, __ATOMIC_RELEASE);
    }
    UINT32 written = 0;
    NDIS_STATUS st = w->request(w->adapter, WDI_OID(msg), port, buf, sizeof(*h) + tlen, CMD_BUF, &written);
    if (rlen) *rlen = written > sizeof(*h) ? written - sizeof(*h) : 0;
    if (st == NDIS_STATUS_SUCCESS && written >= sizeof(*h) && h->Status != NDIS_STATUS_SUCCESS) st = h->Status;
    if (done_id && st == NDIS_STATUS_SUCCESS) {
        INT64 t = -(INT64)wait_s * 10000000;
        if (nt_KeWaitForSingleObject(&w->task_done, 0, 0, 0, &t) != NT_STATUS_SUCCESS) {
            kprintf("[WDI] %s: task %u did not complete in %u s\n", w->info.service, msg, wait_s);
            st = NDIS_STATUS_FAILURE;
        } else {
            st = w->task_status;
        }
    }
    if (done_id) __atomic_store_n(&w->task_tid, 0, __ATOMIC_RELEASE);
    return st;
}

static void mac_str(const UINT8 *m, char *out)
{
    ksnprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

/* OID_WDI_GET_ADAPTER_CAPABILITIES: what the adapter is */
static bool get_capabilities(WdiHost *w, UINT8 *buf)
{
    UINT32 n = 0;
    NDIS_STATUS st = command(w, WDI_GET_ADAPTER_CAPABILITIES, WDI_PORT_ID_ADAPTER, buf, 0, 0, &n, 0);
    if (st != NDIS_STATUS_SUCCESS) {
        kprintf("[WDI] %s: adapter capabilities failed (0x%08x)\n", w->info.service, (UINT32)st);
        return false;
    }
    const UINT8 *m = buf + sizeof(WDI_MESSAGE_HEADER);
    UINT32 ilen;
    const UINT8 *ia = tlv_find(m, n, WDI_TLV_INTERFACE_ATTRIBUTES, &ilen);
    WDI_INTERFACE_CAPABILITIES ic;
    if (!ia || !tlv_get(ia, ilen, WDI_TLV_INTERFACE_CAPABILITIES, &ic, sizeof(ic))) {
        kprintf("[WDI] %s: adapter capabilities have no interface capabilities\n", w->info.service);
        return false;
    }
    memcpy(w->info.mac, ic.Address, 6);
    w->info.hw_radio = ic.HardwareRadioState;
    w->info.sw_radio = ic.SoftwareRadioState;
    w->info.max_rx_kbps = ic.MaxRxRate;
    w->info.max_tx_kbps = ic.MaxTxRate;
    UINT32 fl;
    const UINT8 *fw = tlv_find(ia, ilen, WDI_TLV_FIRMWARE_VERSION, &fl);
    if (fw) {
        if (fl >= sizeof(w->info.firmware)) fl = sizeof(w->info.firmware) - 1;
        memcpy(w->info.firmware, fw, fl);
        w->info.firmware[fl] = 0;
    }
    /* (one WDI_TLV_BAND_INFO per band) */
    UINT32 off = 0;
    while (off + sizeof(WDI_TLV_HEADER) <= n) {
        const WDI_TLV_HEADER *t = (const WDI_TLV_HEADER *)(m + off);
        if (off + sizeof(*t) + t->Length > n) break;
        WDI_BAND_CAPABILITIES bc;
        if (t->Type == WDI_TLV_BAND_INFO &&
            tlv_get(m + off + sizeof(*t), t->Length, WDI_TLV_BAND_CAPABILITIES, &bc, sizeof(bc)) && bc.BandState &&
            bc.BandID < 32)
            w->info.bands |= 1u << bc.BandID;
        off += sizeof(*t) + t->Length;
    }
    char mac[18];
    mac_str(w->info.mac, mac);
    kprintf("[WDI] %s: adapter capabilities: MAC %s, firmware %s, radio %s/%s, bands%s%s%s\n", w->info.service, mac,
            w->info.firmware[0] ? w->info.firmware : "(none)", w->info.hw_radio ? "on" : "off",
            w->info.sw_radio ? "on" : "off", w->info.bands & (1u << WDI_BAND_ID_2400) ? " 2.4" : "",
            w->info.bands & (1u << WDI_BAND_ID_5000) ? " 5" : "", w->info.bands & (1u << WDI_BAND_ID_6000) ? " 6" : "");
    return true;
}

/* OID_WDI_TASK_CREATE_PORT: the station port scans and connections use */
static bool create_port(WdiHost *w, UINT8 *buf)
{
    WDI_CREATE_PORT_PARAMETERS p = { WDI_OPERATION_MODE_STA, 0 };
    UINT32 tlen = tlv_put(buf + sizeof(WDI_MESSAGE_HEADER), 0, WDI_TLV_CREATE_PORT_PARAMETERS, &p, sizeof(p));
    memset(&w->port_attr, 0, sizeof(w->port_attr));
    NDIS_STATUS st = command(w, WDI_TASK_CREATE_PORT, WDI_PORT_ID_ADAPTER, buf, tlen,
                             WDI_INDICATION_CREATE_PORT_COMPLETE, NULL, TASK_WAIT_S);
    if (st != NDIS_STATUS_SUCCESS) {
        kprintf("[WDI] %s: creating the station port failed (0x%08x)\n", w->info.service, (UINT32)st);
        return false;
    }
    w->port_id = w->port_attr.PortNumber;
    w->info.port = true;
    char mac[18];
    mac_str(w->port_attr.MacAddress, mac);
    kprintf("[WDI] %s: station port %u created (MAC %s)\n", w->info.service, w->port_id, mac);
    return true;
}

static void delete_port(WdiHost *w, UINT8 *buf)
{
    WDI_DELETE_PORT_PARAMETERS p = { w->port_id };
    UINT32 tlen = tlv_put(buf + sizeof(WDI_MESSAGE_HEADER), 0, WDI_TLV_DELETE_PORT_PARAMETERS, &p, sizeof(p));
    NDIS_STATUS st = command(w, WDI_TASK_DELETE_PORT, WDI_PORT_ID_ADAPTER, buf, tlen,
                             WDI_INDICATION_DELETE_PORT_COMPLETE, NULL, TASK_WAIT_S);
    kprintf("[WDI] %s: station port %u deleted (0x%08x)\n", w->info.service, w->port_id, (UINT32)st);
    w->info.port = false;
}

/* -----------------------------------------------------------------------
 * BSS entries: what an access point's beacon or probe response says
 * ----------------------------------------------------------------------- */

static const char *akm_auth(UINT8 akm)
{
    switch (akm) {
    case 1: case 3: case 5: return "WPA2-Enterprise";
    case 2: case 4: case 6: return "WPA2-Personal";
    case 8: case 9:         return "WPA3-Personal";
    case 12: case 13:       return "WPA3-Enterprise";
    case 18:                return "OWE";
    default:                return "WPA2";
    }
}

/* The frame's fixed fields and elements (WDI_TLV_BEACON_FRAME or
 * WDI_TLV_PROBE_RESPONSE_FRAME: the frame body, timestamp first; a frame
 * that still has its 24-byte MAC header is skipped past it) */
static void parse_frame(WlanBss *b, const UINT8 *f, UINT32 n)
{
    if (n >= 36 && (f[0] == 0x80 || f[0] == 0x50) && f[1] == 0 && !memcmp(f + 16, b->bssid, 6)) {
        f += 24;
        n -= 24;
    }
    if (n < 12) return;
    UINT16 cap = (UINT16)(f[10] | f[11] << 8);
    b->privacy = cap & 0x10;
    bool rsn = false, wpa = false, ht = false, vht = false, he = false;
    for (UINT32 off = 12; off + 2 <= n;) {
        UINT8 id = f[off], len = f[off + 1];
        const UINT8 *e = f + off + 2;
        if (off + 2 + len > n) break;
        if (id == 0 && len <= 32) {                                  /* SSID */
            memcpy(b->ssid, e, len);
            b->ssid[len] = 0;
        } else if (id == 3 && len >= 1 && !b->channel) {             /* DS Parameter Set */
            b->channel = e[0];
        } else if (id == 48 && len >= 8) {                           /* RSN */
            rsn = true;
            UINT32 p = 6;                                            /* (version, group cipher) */
            UINT16 np = (UINT16)(e[p] | e[p + 1] << 8);
            p += 2;
            b->cipher = "CCMP";
            for (UINT16 i = 0; i < np && p + 4 <= len; i++, p += 4)
                if (e[p + 3] == 8 || e[p + 3] == 9) b->cipher = "GCMP";
                else if (e[p + 3] == 2 && np == 1) b->cipher = "TKIP";
            if (p + 2 <= len) {
                UINT16 na = (UINT16)(e[p] | e[p + 1] << 8);
                p += 2;
                b->auth = NULL;
                for (UINT16 i = 0; i < na && p + 4 <= len; i++, p += 4) {
                    const char *a = akm_auth(e[p + 3]);
                    /* (an AP offering SAE and PSK, WPA3 transition mode, shows as WPA3) */
                    if (!b->auth || !strcmp(a, "WPA3-Personal")) b->auth = a;
                }
            }
        } else if (id == 221 && len >= 4 && e[0] == 0x00 && e[1] == 0x50 && e[2] == 0xF2 && e[3] == 1) {
            wpa = true;                                              /* (WPA, before RSN) */
        } else if (id == 45) {
            ht = true;
        } else if (id == 191) {
            vht = true;
        } else if (id == 255 && len >= 1 && e[0] == 35) {            /* (element ID extension: HE capabilities) */
            he = true;
        }
        off += 2u + len;
    }
    if (!rsn) {
        b->auth = wpa ? "WPA-Personal" : "Open";
        b->cipher = wpa ? "TKIP" : b->privacy ? "WEP" : "None";
    }
    if (!b->auth) b->auth = "WPA2";
    b->radio = he ? "802.11ax" : vht ? "802.11ac" : ht ? "802.11n" : b->band == WDI_BAND_ID_2400 ? "802.11g" : "802.11a";
}

/* One WDI_TLV_BSS_ENTRY into the list (replacing the entry with its BSSID) */
static void add_bss(WdiHost *w, const UINT8 *v, UINT32 n)
{
    WlanBss b;
    memset(&b, 0, sizeof(b));
    if (!tlv_get(v, n, WDI_TLV_BSSID, b.bssid, 6)) return;
    WDI_SIGNAL_INFO si;
    if (tlv_get(v, n, WDI_TLV_BSS_ENTRY_SIGNAL_INFO, &si, sizeof(si))) {
        b.rssi = si.RSSI;
        b.quality = si.LinkQuality > 100 ? 100 : si.LinkQuality;
    }
    WDI_BSS_ENTRY_CHANNEL_INFO ci;
    if (tlv_get(v, n, WDI_TLV_BSS_ENTRY_CHANNEL_INFO, &ci, sizeof(ci))) {
        b.channel = ci.ChannelNumber;
        b.band = ci.BandId;
    }
    UINT32 fl;
    const UINT8 *f = tlv_find(v, n, WDI_TLV_PROBE_RESPONSE_FRAME, &fl);
    if (!f) f = tlv_find(v, n, WDI_TLV_BEACON_FRAME, &fl);
    if (f) parse_frame(&b, f, fl);
    else { b.auth = "Open"; b.cipher = "None"; b.radio = "802.11"; }
    UINT8 old = nt_KeAcquireSpinLockRaiseToDpc(&w->bss_lock);
    int i;
    for (i = 0; i < w->nbss && memcmp(w->bss[i].bssid, b.bssid, 6); i++) {}
    if (i < WLAN_MAX_BSS) {
        w->bss[i] = b;
        if (i == w->nbss) w->nbss++;
    }
    nt_KeReleaseSpinLock(&w->bss_lock, old);
}

/* -----------------------------------------------------------------------
 * Indications (from NdisMIndicateStatusEx, at up to DISPATCH_LEVEL)
 * ----------------------------------------------------------------------- */
void WdiIndicate(void *adapter, UINT32 code, const void *buf, UINT32 len)
{
    WdiHost *w = NULL;
    for (int i = 0; i < WDI_MAX_ADAPTERS && !w; i++)
        if (g_hosts[i].used && g_hosts[i].adapter == adapter) w = &g_hosts[i];
    if (!w || len < sizeof(WDI_MESSAGE_HEADER)) return;
    const WDI_MESSAGE_HEADER *h = buf;
    const UINT8 *m = (const UINT8 *)buf + sizeof(*h);
    UINT32 n = len - sizeof(*h), id = code & 0xFFFF;
    if (id == WDI_INDICATION_BSS_ENTRY_LIST) {
        UINT32 off = 0;
        while (off + sizeof(WDI_TLV_HEADER) <= n) {
            const WDI_TLV_HEADER *t = (const WDI_TLV_HEADER *)(m + off);
            if (off + sizeof(*t) + t->Length > n) break;
            if (t->Type == WDI_TLV_BSS_ENTRY) add_bss(w, m + off + sizeof(*t), t->Length);
            off += sizeof(*t) + t->Length;
        }
    }
    UINT32 tid = __atomic_load_n(&w->task_tid, __ATOMIC_ACQUIRE);
    if (tid && h->TransactionId == tid && id == w->task_done_id) {
        if (id == WDI_INDICATION_CREATE_PORT_COMPLETE)
            tlv_get(m, n, WDI_TLV_PORT_ATTRIBUTES, &w->port_attr, sizeof(w->port_attr));
        w->task_status = h->Status;
        nt_KeSetEvent(&w->task_done, 0, 0);
    }
}

/* -----------------------------------------------------------------------
 * Adapters (ndis.c attaches one after its open, detaches it before the close)
 * ----------------------------------------------------------------------- */
WdiHost *WdiAttach(void *adapter, const char *service, const char *desc, WdiRequestFn request)
{
    WdiHost *w = NULL;
    UINT8 old = nt_KeAcquireSpinLockRaiseToDpc(&g_lock);
    for (int i = 0; i < WDI_MAX_ADAPTERS && !w; i++)
        if (!g_hosts[i].used) { w = &g_hosts[i]; w->used = true; }
    nt_KeReleaseSpinLock(&g_lock, old);
    UINT8 *buf = w ? kmalloc(CMD_BUF) : NULL;
    if (!buf) {
        if (w) w->used = false;
        kprintf("[WDI] %s: no room for another Wi-Fi adapter\n", service);
        return NULL;
    }
    memset(&w->info, 0, sizeof(w->info));
    strncpy(w->info.service, service, sizeof(w->info.service) - 1);
    strncpy(w->info.description, desc && desc[0] ? desc : service, sizeof(w->info.description) - 1);
    w->request = request;
    w->nbss = 0;
    nt_KeInitializeEvent(&w->cmd_lock, NT_SynchronizationEvent, 0);   /* (taken: the bring-up below holds it) */
    nt_KeInitializeEvent(&w->task_done, NT_NotificationEvent, 0);
    w->bss_lock = 0;
    w->adapter = adapter;
    if (get_capabilities(w, buf)) create_port(w, buf);
    unlock(w);
    kfree(buf);
    return w;
}

void WdiDetach(WdiHost *w)
{
    if (!w) return;
    lock(w);                    /* (waits for a scan in progress) */
    UINT8 *buf = kmalloc(CMD_BUF);
    if (buf && w->info.port) delete_port(w, buf);
    kfree(buf);
    w->adapter = NULL;
    w->info.port = false;
    unlock(w);
    w->used = false;
}

/* The @i'th attached adapter, locked, or NULL */
static WdiHost *host_locked(int i)
{
    for (int k = 0, n = 0; k < WDI_MAX_ADAPTERS; k++) {
        WdiHost *w = &g_hosts[k];
        if (!w->used || !w->adapter || n++ != i) continue;
        lock(w);
        if (w->adapter) return w;
        unlock(w);
        return NULL;
    }
    return NULL;
}

int WlanCount(void)
{
    int n = 0;
    for (int i = 0; i < WDI_MAX_ADAPTERS; i++) n += g_hosts[i].used && g_hosts[i].adapter;
    return n;
}

bool WlanGetInfo(int i, WlanInfo *out)
{
    WdiHost *w = host_locked(i);
    if (!w) return false;
    *out = w->info;
    unlock(w);
    return true;
}

int WlanScan(int i, WlanBss *out, int max, UINT32 *status)
{
    if (status) *status = (UINT32)NDIS_STATUS_FAILURE;
    UINT8 *buf = kmalloc(CMD_BUF);
    if (!buf) return -1;
    WdiHost *w = host_locked(i);
    if (!w || !w->info.port) {
        if (w) unlock(w);
        kfree(buf);
        return -1;
    }
    UINT8 old = nt_KeAcquireSpinLockRaiseToDpc(&w->bss_lock);
    w->nbss = 0;
    nt_KeReleaseSpinLock(&w->bss_lock, old);
    /* every BSS (the wildcard BSSID and SSID) on every channel, live */
    static const UINT8 wildcard[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    WDI_SCAN_MODE mode = { 1, WDI_SCAN_TYPE_AUTO, 1, WDI_SCAN_TRIGGER_MANUAL };
    WDI_SCAN_DWELL_TIME dwell = { 20, 110, SCAN_MAX_MS };
    UINT8 *t = buf + sizeof(WDI_MESSAGE_HEADER);
    UINT32 tlen = tlv_put(t, 0, WDI_TLV_BSSID, wildcard, 6);
    tlen = tlv_put(t, tlen, WDI_TLV_SSID, NULL, 0);
    tlen = tlv_put(t, tlen, WDI_TLV_SCAN_MODE, &mode, sizeof(mode));
    tlen = tlv_put(t, tlen, WDI_TLV_SCAN_DWELL_TIME, &dwell, sizeof(dwell));
    NDIS_STATUS st = command(w, WDI_TASK_SCAN, w->port_id, buf, tlen, WDI_INDICATION_SCAN_COMPLETE, NULL,
                             SCAN_MAX_MS / 1000 + TASK_WAIT_S);
    int n = -1;
    if (st == NDIS_STATUS_SUCCESS) {
        old = nt_KeAcquireSpinLockRaiseToDpc(&w->bss_lock);
        n = w->nbss < max ? w->nbss : max;
        memcpy(out, w->bss, (size_t)n * sizeof(*out));
        nt_KeReleaseSpinLock(&w->bss_lock, old);
        /* strongest first, as netsh lists them */
        for (int a = 1; a < n; a++)
            for (int b = a; b > 0 && out[b].quality > out[b - 1].quality; b--) {
                WlanBss x = out[b];
                out[b] = out[b - 1];
                out[b - 1] = x;
            }
    }
    kprintf("[WDI] %s: scan on port %u: 0x%08x, %d access point(s)\n", w->info.service, w->port_id, (UINT32)st, n < 0 ? 0 : n);
    if (status) *status = (UINT32)st;
    unlock(w);
    kfree(buf);
    return n;
}

/* -----------------------------------------------------------------------
 * A scan in the background, for netsh (the Terminal does not wait)
 * ----------------------------------------------------------------------- */
static volatile bool g_scan_busy, g_scan_done;
static int           g_scan_adapter, g_scan_n;
static UINT32        g_scan_status;
static WlanBss       g_scan_bss[WLAN_MAX_BSS];

static void scan_thread(void *arg)
{
    (void)arg;
    UINT32 st = 0;
    g_scan_n = WlanScan(g_scan_adapter, g_scan_bss, WLAN_MAX_BSS, &st);
    g_scan_status = st;
    __atomic_store_n(&g_scan_done, true, __ATOMIC_RELEASE);
}

bool WlanScanStart(int i)
{
    /* (a result nobody collected, its netsh stopped with Ctrl+C: dropped) */
    if (__atomic_load_n(&g_scan_done, __ATOMIC_ACQUIRE)) {
        g_scan_done = false;
        __atomic_store_n(&g_scan_busy, false, __ATOMIC_RELEASE);
    }
    if (i < 0 || i >= WlanCount() || __atomic_exchange_n(&g_scan_busy, true, __ATOMIC_ACQ_REL)) return false;
    g_scan_adapter = i;
    g_scan_done = false;
    if (!sched_create_thread("wlanscan", scan_thread, NULL, PRIO_SERVICE)) {
        g_scan_busy = false;
        return false;
    }
    return true;
}

bool WlanScanDone(WlanBss *out, int max, int *n, UINT32 *status)
{
    if (!__atomic_load_n(&g_scan_done, __ATOMIC_ACQUIRE)) return false;
    *n = g_scan_n < max ? g_scan_n : max;
    if (*n > 0) memcpy(out, g_scan_bss, (size_t)*n * sizeof(*out));
    if (status) *status = g_scan_status;
    g_scan_done = false;
    __atomic_store_n(&g_scan_busy, false, __ATOMIC_RELEASE);
    return true;
}
