/*
 * wevtapi.dll — the Windows Event Log API (EvtQuery and its handles).
 *
 * NovaOS keeps no event channels: what programs report (advapi32's
 * ReportEvent) goes to the kernel log.  So a query of a channel finds no
 * events, the answer Windows gives for a channel that is empty: EvtQuery
 * hands out a result set and EvtNext ends it at once with
 * ERROR_NO_MORE_ITEMS.  Chromium queries the System log this way at start
 * (to tell whether the last shutdown was clean).
 */
#include <windows.h>

#define WEVTAPI __declspec(dllexport)
#define ERROR_NO_MORE_ITEMS_ 259
#define EvtQueryChannelPath 0x1
#define EvtQueryFilePath    0x2

enum { H_QUERY = 0x51455645, H_RENDER = 0x52455645 };     /* "EVEQ", "EVER" */
typedef struct { DWORD magic; } EvtObj;

static HANDLE make(DWORD magic)
{
    EvtObj *o = HeapAlloc(GetProcessHeap(), 0, sizeof(*o));
    if (!o) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    o->magic = magic;
    return o;
}
static EvtObj *obj(HANDLE h)
{
    EvtObj *o = h;
    return o && (o->magic == H_QUERY || o->magic == H_RENDER) ? o : 0;
}

WEVTAPI HANDLE WINAPI EvtQuery(HANDLE session, LPCWSTR path, LPCWSTR query, DWORD flags)
{
    (void)query;
    if (session) { SetLastError(ERROR_NOT_SUPPORTED); return 0; }      /* (remote sessions) */
    DWORD kind = flags & 3;
    if ((kind != EvtQueryChannelPath && kind != EvtQueryFilePath) || (!path && !query)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (kind == EvtQueryFilePath && GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return 0;
    }
    return make(H_QUERY);
}

WEVTAPI BOOL WINAPI EvtNext(HANDLE results, DWORD n, HANDLE *events, DWORD timeout, DWORD flags, DWORD *returned)
{
    (void)timeout; (void)flags;
    EvtObj *o = obj(results);
    if (!o || o->magic != H_QUERY) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (!n || !events || !returned) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    *returned = 0;
    SetLastError(ERROR_NO_MORE_ITEMS_);
    return FALSE;
}

WEVTAPI HANDLE WINAPI EvtCreateRenderContext(DWORD n, LPCWSTR *paths, DWORD flags)
{
    if (flags > 2 || (flags == 0 && n && !paths)) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    return make(H_RENDER);
}

/* Only an event can be rendered, and no query yields one */
WEVTAPI BOOL WINAPI EvtRender(HANDLE context, HANDLE fragment, DWORD flags, DWORD size, void *buf, DWORD *used, DWORD *count)
{
    (void)context; (void)flags; (void)size; (void)buf;
    if (used) *used = 0;
    if (count) *count = 0;
    (void)fragment;
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}

WEVTAPI BOOL WINAPI EvtClose(HANDLE h)
{
    EvtObj *o = obj(h);
    if (!o) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    o->magic = 0;
    HeapFree(GetProcessHeap(), 0, o);
    return TRUE;
}
