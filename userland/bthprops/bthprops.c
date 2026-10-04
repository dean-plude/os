/*
 * bthprops.cpl — Bluetooth's classic API (Windows keeps it in the
 * Bluetooth Control Panel item, which forwards to BluetoothApis.dll).
 * NovaOS has no Bluetooth driver, so there is no radio: the radio and
 * device searches end at once with ERROR_NO_MORE_ITEMS, as on a PC whose
 * Bluetooth is absent or switched off, and nothing else has a radio or
 * search handle to act on.  Programs such as Chromium's (Qt WebEngine) then
 * report Bluetooth as unavailable.
 *
 * The Service Discovery Protocol parsers (BluetoothSdp*) need no radio:
 * they read SDP records, the big-endian data elements of the Bluetooth
 * Core Specification (Vol 3, Part B, 3), and work in full.
 */
#include <windows.h>

void *memset(void *d, int c, size_t n);

#define BTAPI __declspec(dllexport)
#define ERROR_REVISION_MISMATCH_ 1306

typedef struct { DWORD dwSize; } FIND_RADIO_PARAMS_;
typedef struct { DWORD dwSize; } SIZED_;               /* (every Bluetooth structure starts with dwSize) */

static BOOL bad_size(const void *p, DWORD want)
{
    if (!p) { SetLastError(ERROR_INVALID_PARAMETER); return TRUE; }
    if (((const SIZED_ *)p)->dwSize != want) { SetLastError(ERROR_REVISION_MISMATCH_); return TRUE; }
    return FALSE;
}

BTAPI HANDLE WINAPI BluetoothFindFirstRadio(const FIND_RADIO_PARAMS_ *params, HANDLE *radio)
{
    if (radio) *radio = 0;
    if (!radio) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    if (bad_size(params, sizeof(FIND_RADIO_PARAMS_))) return 0;
    SetLastError(ERROR_NO_MORE_ITEMS);
    return 0;
}
BTAPI BOOL WINAPI BluetoothFindNextRadio(HANDLE find, HANDLE *radio)
{ (void)find; if (radio) *radio = 0; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
BTAPI BOOL WINAPI BluetoothFindRadioClose(HANDLE find) { (void)find; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
BTAPI DWORD WINAPI BluetoothGetRadioInfo(HANDLE radio, void *info) { (void)radio; return info ? ERROR_INVALID_HANDLE : ERROR_INVALID_PARAMETER; }

/* BLUETOOTH_DEVICE_SEARCH_PARAMS is 40 bytes on x64 (32 on x86) */
BTAPI HANDLE WINAPI BluetoothFindFirstDevice(const void *params, void *info)
{
    if (!params || !info) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    SetLastError(ERROR_NO_MORE_ITEMS);
    return 0;
}
BTAPI BOOL WINAPI BluetoothFindNextDevice(HANDLE find, void *info) { (void)find; (void)info; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
BTAPI BOOL WINAPI BluetoothFindDeviceClose(HANDLE find) { (void)find; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
BTAPI DWORD WINAPI BluetoothGetDeviceInfo(HANDLE radio, void *info) { (void)radio; return info ? ERROR_NOT_FOUND : ERROR_INVALID_PARAMETER; }
BTAPI DWORD WINAPI BluetoothRemoveDevice(const void *addr) { return addr ? ERROR_NOT_FOUND : ERROR_INVALID_PARAMETER; }

/* A radio's switches: with no radio, none is connectable or discoverable
 * and none can be made so */
BTAPI BOOL WINAPI BluetoothIsConnectable(HANDLE radio) { (void)radio; return FALSE; }
BTAPI BOOL WINAPI BluetoothIsDiscoverable(HANDLE radio) { (void)radio; return FALSE; }
BTAPI BOOL WINAPI BluetoothEnableDiscovery(HANDLE radio, BOOL on) { (void)radio; (void)on; return FALSE; }
BTAPI BOOL WINAPI BluetoothEnableIncomingConnections(HANDLE radio, BOOL on) { (void)radio; (void)on; return FALSE; }

/* -----------------------------------------------------------------------
 * SDP records
 * ----------------------------------------------------------------------- */
enum { T_NIL, T_UINT, T_INT, T_UUID, T_STRING, T_BOOLEAN, T_SEQUENCE, T_ALTERNATIVE, T_URL };

typedef struct {
    int type, specificType;                             /* SDP_TYPE, SDP_SPECIFICTYPE */
    union {
        struct { ULONGLONG lo; LONGLONG hi; } int128;
        LONGLONG int64; LONG int32; SHORT int16; CHAR int8;
        struct { ULONGLONG lo, hi; } uint128;
        ULONGLONG uint64; ULONG uint32; USHORT uint16; UCHAR uint8;
        UCHAR booleanVal;
        GUID uuid128; ULONG uuid32; USHORT uuid16;
        struct { LPBYTE value; ULONG length; } string, url, sequence, alternative;
    } data;
} SDP_ELEMENT_DATA_;

static ULONGLONG be(const BYTE *p, int n) { ULONGLONG v = 0; for (int i = 0; i < n; i++) v = v << 8 | p[i]; return v; }

/* The element at @p (@n bytes left): its type, the size index, where its
 * data starts and how long the data and the whole element are; 0 if it
 * does not fit or is malformed */
static int element(const BYTE *p, ULONG n, int *type, int *szi, const BYTE **data, ULONG *dlen, ULONG *total)
{
    if (!p || n < 1) return 0;
    int t = p[0] >> 3, si = p[0] & 7;
    ULONG hdr = 1, len;
    if (t > T_URL) return 0;
    if (si < 5) {
        static const ULONG fixed[5] = { 1, 2, 4, 8, 16 };
        len = t == T_NIL ? 0 : fixed[si];
        if (t == T_NIL && si) return 0;
        if (t == T_STRING || t == T_SEQUENCE || t == T_ALTERNATIVE || t == T_URL) return 0;
        if (t == T_BOOLEAN && si) return 0;
        if (t == T_UUID && si != 1 && si != 2 && si != 4) return 0;
    } else {
        int k = si == 5 ? 1 : si == 6 ? 2 : 4;
        if (n < 1 + (ULONG)k) return 0;
        if (t != T_STRING && t != T_SEQUENCE && t != T_ALTERNATIVE && t != T_URL) return 0;
        len = (ULONG)be(p + 1, k);
        hdr += (ULONG)k;
    }
    if (len > n - hdr) return 0;
    *type = t; *szi = si; *data = p + hdr; *dlen = len; *total = hdr + len;
    return 1;
}

static DWORD fill(const BYTE *p, ULONG n, SDP_ELEMENT_DATA_ *out)
{
    int t, si;
    const BYTE *d;
    ULONG len, total;
    if (!out) return ERROR_INVALID_PARAMETER;
    if (!element(p, n, &t, &si, &d, &len, &total)) return ERROR_INVALID_PARAMETER;
    memset(out, 0, sizeof(*out));
    out->type = t;
    out->specificType = t == T_UINT || t == T_INT || t == T_UUID ? (si << 8) | (t << 4) : 0;
    switch (t) {
    case T_UINT:
        if (si == 0) out->data.uint8 = d[0];
        else if (si == 1) out->data.uint16 = (USHORT)be(d, 2);
        else if (si == 2) out->data.uint32 = (ULONG)be(d, 4);
        else if (si == 3) out->data.uint64 = be(d, 8);
        else { out->data.uint128.hi = be(d, 8); out->data.uint128.lo = be(d + 8, 8); }
        break;
    case T_INT:
        if (si == 0) out->data.int8 = (CHAR)d[0];
        else if (si == 1) out->data.int16 = (SHORT)be(d, 2);
        else if (si == 2) out->data.int32 = (LONG)be(d, 4);
        else if (si == 3) out->data.int64 = (LONGLONG)be(d, 8);
        else { out->data.int128.hi = (LONGLONG)be(d, 8); out->data.int128.lo = be(d + 8, 8); }
        break;
    case T_UUID:
        if (si == 1) out->data.uuid16 = (USHORT)be(d, 2);
        else if (si == 2) out->data.uuid32 = (ULONG)be(d, 4);
        else {
            GUID *g = &out->data.uuid128;
            g->Data1 = (ULONG)be(d, 4); g->Data2 = (USHORT)be(d + 4, 2); g->Data3 = (USHORT)be(d + 6, 2);
            for (int i = 0; i < 8; i++) g->Data4[i] = d[8 + i];
        }
        break;
    case T_BOOLEAN: out->data.booleanVal = d[0]; break;
    case T_STRING: out->data.string.value = (LPBYTE)d; out->data.string.length = len; break;
    case T_URL: out->data.url.value = (LPBYTE)d; out->data.url.length = len; break;
    case T_SEQUENCE: case T_ALTERNATIVE:                /* (the element itself, header included) */
        out->data.sequence.value = (LPBYTE)p; out->data.sequence.length = total; break;
    }
    return ERROR_SUCCESS;
}

BTAPI DWORD WINAPI BluetoothSdpGetElementData(LPBYTE stream, ULONG n, SDP_ELEMENT_DATA_ *out)
{
    return fill(stream, n, out);
}

/* The elements of a sequence or alternative, one per call: *@pos is 0 to
 * start and then where the next one is */
BTAPI DWORD WINAPI BluetoothSdpGetContainerElementData(LPBYTE stream, ULONG n, HANDLE *pos, SDP_ELEMENT_DATA_ *out)
{
    int t, si;
    const BYTE *d;
    ULONG len, total;
    if (!pos || !out || !element(stream, n, &t, &si, &d, &len, &total) || (t != T_SEQUENCE && t != T_ALTERNATIVE))
        return ERROR_INVALID_PARAMETER;
    const BYTE *at = *pos ? (const BYTE *)*pos : d, *end = d + len;
    if (at < d || at > end) return ERROR_INVALID_PARAMETER;
    if (at == end) return ERROR_NO_MORE_ITEMS;
    int et, esi;
    const BYTE *ed;
    ULONG elen, etotal;
    if (!element(at, (ULONG)(end - at), &et, &esi, &ed, &elen, &etotal)) return ERROR_INVALID_PARAMETER;
    DWORD e = fill(at, etotal, out);
    if (!e) *pos = (HANDLE)(at + etotal);
    return e;
}

/* A record is a sequence of attribute ID (16-bit unsigned) and value pairs */
typedef BOOL (CALLBACK *SDP_ENUM_CB_)(ULONG id, LPBYTE value, ULONG n, LPVOID param);

static DWORD walk(LPBYTE rec, ULONG n, int want, SDP_ENUM_CB_ cb, LPVOID param, LPBYTE *found, ULONG *flen)
{
    int t, si;
    const BYTE *d;
    ULONG len, total;
    if (!element(rec, n, &t, &si, &d, &len, &total) || t != T_SEQUENCE) return ERROR_INVALID_PARAMETER;
    for (const BYTE *p = d, *end = d + len; p < end; ) {
        int it, isi, vt, vsi;
        const BYTE *id, *vd;
        ULONG il, itot, vl, vtot;
        if (!element(p, (ULONG)(end - p), &it, &isi, &id, &il, &itot) || it != T_UINT || isi != 1) return ERROR_INVALID_PARAMETER;
        p += itot;
        if (!element(p, (ULONG)(end - p), &vt, &vsi, &vd, &vl, &vtot)) return ERROR_INVALID_PARAMETER;
        USHORT a = (USHORT)be(id, 2);
        if (want >= 0 && a == want) { *found = (LPBYTE)p; *flen = vtot; return ERROR_SUCCESS; }
        if (cb && !cb(a, (LPBYTE)p, vtot, param)) return ERROR_SUCCESS;
        p += vtot;
    }
    return want >= 0 ? ERROR_FILE_NOT_FOUND : ERROR_SUCCESS;
}

BTAPI DWORD WINAPI BluetoothSdpGetAttributeValue(LPBYTE rec, ULONG n, USHORT id, SDP_ELEMENT_DATA_ *out)
{
    LPBYTE v = 0;
    ULONG vl = 0;
    if (!out) return ERROR_INVALID_PARAMETER;
    DWORD e = walk(rec, n, id, 0, 0, &v, &vl);
    return e ? e : fill(v, vl, out);
}

BTAPI BOOL WINAPI BluetoothSdpEnumAttributes(LPBYTE rec, ULONG n, SDP_ENUM_CB_ cb, LPVOID param)
{
    if (!cb) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    DWORD e = walk(rec, n, -1, cb, param, 0, 0);
    if (e) { SetLastError(e); return FALSE; }
    return TRUE;
}
