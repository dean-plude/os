/*
 * wlan.h — the Wi-Fi adapters NovaOS's WDI upper edge (kernel/nt/wdi.c)
 * drives, for the rest of the kernel: netsh wlan (kernel/apps/terminal.c)
 * lists them and scans with them
 */
#pragma once

#include "../include/types.h"

#define WLAN_MAX_BSS 32

/* An access point a scan found (one BSS) */
typedef struct {
    UINT8  bssid[6];
    char   ssid[33];
    INT32  rssi;            /* dBm */
    UINT32 quality;         /* 0-100: netsh's "Signal" */
    UINT32 channel;
    UINT32 band;            /* WDI_BAND_ID_*: 2.4, 5 or 6 GHz */
    bool   privacy;         /* the capability field's Privacy bit */
    const char *auth;       /* "Open", "WPA2-Personal", "WPA3-Personal", ... (from its RSN element) */
    const char *cipher;     /* "None", "CCMP", "GCMP", "TKIP", "WEP" */
    const char *radio;      /* "802.11n", "802.11ac", "802.11ax" or "802.11g" (from its elements) */
} WlanBss;

typedef struct {
    char   description[96]; /* the device's DeviceDesc */
    char   service[64];
    char   firmware[64];
    UINT8  mac[6];
    bool   hw_radio, sw_radio;
    bool   port;            /* its station port exists (WDI_TASK_CREATE_PORT) */
    UINT32 max_rx_kbps, max_tx_kbps;
    UINT32 bands;           /* bit (1 << WDI_BAND_ID_*) per band it has enabled */
} WlanInfo;

/* How many Wi-Fi adapters are open; the @i'th one's details */
int  WlanCount(void);
bool WlanGetInfo(int i, WlanInfo *out);
/* Scan with the @i'th adapter (OID_WDI_TASK_SCAN): up to @max access points
 * it found, strongest first, or -1 if the scan failed (*@status says why) */
int  WlanScan(int i, WlanBss *out, int max, UINT32 *status);
/* The same in the background (netsh's): start one (false if there is no
 * such adapter or a scan is running), then poll for its outcome (false
 * while it runs; then *@n is WlanScan's answer) */
bool WlanScanStart(int i);
bool WlanScanDone(WlanBss *out, int max, int *n, UINT32 *status);

/* ndis.c's side.  @request sends an OID_WDI_* method request to the
 * miniport (its buffer: @in_len bytes of message, room for @out_len) and
 * waits for it; it returns the NDIS status and the bytes written. */
typedef struct WdiHost WdiHost;
typedef INT32 (*WdiRequestFn)(void *adapter, UINT32 oid, UINT16 port, void *buf, UINT32 in_len, UINT32 out_len,
                              UINT32 *written);
/* After MiniportWdiOpenAdapter: the adapter's capabilities and station
 * port; NULL if there is no room for it */
WdiHost *WdiAttach(void *adapter, const char *service, const char *desc, WdiRequestFn request);
/* An NDIS_STATUS_WDI_INDICATION_* @adapter's miniport indicated (any IRQL
 * up to DISPATCH_LEVEL; also while WdiAttach is bringing it up) */
void     WdiIndicate(void *adapter, UINT32 code, const void *buf, UINT32 len);
/* Before MiniportWdiCloseAdapter: deletes the port */
void     WdiDetach(WdiHost *w);
