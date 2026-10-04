/*
 * wevtapi.dll — the Windows Event Log API (EvtQuery and its handles).
 *
 * NovaOS keeps no event channels: what programs report (advapi32's
 * ReportEvent) goes to the kernel log.  So a query of a channel finds no
 * events, the answer Windows gives for a channel that is empty: EvtQuery
 * hands out a result set and EvtNext ends it at once with
 * ERROR_NO_MORE_ITEMS.  Chromium queries the System log this way at start
 * (to tell whether the last shutdown was clean).  The same holds for the
 * rest: a subscription is made and never fires, a log opens with no
 * records, a bookmark is kept but no event can move it, the channel and
 * publisher lists are empty and no publisher has metadata to read
 * (Edge's WebView2 browser calls most of these; a missing one was a
 * breakpoint through its delay-load hook).
 */
#include <windows.h>

#define WEVTAPI __declspec(dllexport)
#define ERROR_NO_MORE_ITEMS_ 259
#define EvtQueryChannelPath 0x1
#define EvtQueryFilePath    0x2

#define ERROR_EVT_PUBLISHER_METADATA_NOT_FOUND 15002
enum { H_QUERY = 0x51455645, H_RENDER = 0x52455645,      /* "EVEQ", "EVER" */
       H_BOOKMARK = 0x42455645, H_LOG = 0x4C455645,     /* "EVEB", "EVEL" */
       H_ENUM = 0x4E455645, H_CONFIG = 0x43455645 };    /* "EVEN", "EVEC" */
typedef struct { DWORD magic; } EvtObj;

/* EVT_VARIANT: a value, a count and a type */
typedef struct { ULONGLONG v; DWORD count, type; } EvtVariant;
enum { EvtVarTypeNull = 0, EvtVarTypeUInt32 = 8, EvtVarTypeUInt64 = 10, EvtVarTypeBoolean = 13,
       EvtVarTypeFileTime = 17 };

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
    return o && (o->magic == H_QUERY || o->magic == H_RENDER || o->magic == H_BOOKMARK ||
                 o->magic == H_LOG || o->magic == H_ENUM || o->magic == H_CONFIG) ? o : 0;
}
static EvtObj *obj_of(HANDLE h, DWORD magic)
{
    EvtObj *o = obj(h);
    if (!o || o->magic != magic) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
    return o;
}
/* One EVT_VARIANT into the caller's buffer (@used: the size it needs) */
static BOOL variant(DWORD type, ULONGLONG v, DWORD size, void *buf, DWORD *used)
{
    if (used) *used = sizeof(EvtVariant);
    if (size < sizeof(EvtVariant) || !buf) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    EvtVariant *e = buf;
    e->v = v;
    e->count = 0;
    e->type = type;
    return TRUE;
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
    if (!o || (o->magic != H_QUERY && o->magic != H_ENUM)) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
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

/* A subscription to a channel: the handle is real, no event ever comes
 * (to a callback, an event or EvtNext) */
WEVTAPI HANDLE WINAPI EvtSubscribe(HANDLE session, HANDLE signal, LPCWSTR path, LPCWSTR query, HANDLE bookmark,
                                   void *context, void *callback, DWORD flags)
{
    (void)signal; (void)query; (void)context; (void)callback;
    if (session) { SetLastError(ERROR_NOT_SUPPORTED); return 0; }
    if (!path && !query) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    if ((flags & 3) == 3 && !obj_of(bookmark, H_BOOKMARK)) return 0;   /* EvtSubscribeStartAfterBookmark */
    return make(H_QUERY);
}

WEVTAPI HANDLE WINAPI EvtCreateBookmark(LPCWSTR xml) { (void)xml; return make(H_BOOKMARK); }

/* Only an event moves a bookmark, and there are none */
WEVTAPI BOOL WINAPI EvtUpdateBookmark(HANDLE bookmark, HANDLE event)
{
    (void)event;
    if (!obj_of(bookmark, H_BOOKMARK)) return FALSE;
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}

WEVTAPI HANDLE WINAPI EvtOpenLog(HANDLE session, LPCWSTR path, DWORD flags)
{
    if (session) { SetLastError(ERROR_NOT_SUPPORTED); return 0; }
    if (!path || (flags != EvtQueryChannelPath && flags != EvtQueryFilePath)) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    if (flags == EvtQueryFilePath && GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return 0;
    }
    return make(H_LOG);
}

/* An empty log: no records, no size, not full */
WEVTAPI BOOL WINAPI EvtGetLogInfo(HANDLE log, int id, DWORD size, void *buf, DWORD *used)
{
    static const DWORD type[] = { EvtVarTypeFileTime, EvtVarTypeFileTime, EvtVarTypeFileTime, EvtVarTypeUInt64,
                                  EvtVarTypeUInt32, EvtVarTypeUInt64, EvtVarTypeUInt64, EvtVarTypeBoolean };
    if (!obj_of(log, H_LOG)) return FALSE;
    if (id < 0 || id >= (int)(sizeof(type) / sizeof(type[0]))) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    return variant(type[id], 0, size, buf, used);
}

/* The channel and publisher lists: empty (EvtNextChannelPath and
 * EvtNextPublisherId end at once) */
WEVTAPI HANDLE WINAPI EvtOpenChannelEnum(HANDLE session, DWORD flags)
{
    if (session) { SetLastError(ERROR_NOT_SUPPORTED); return 0; }
    if (flags) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    return make(H_ENUM);
}
WEVTAPI HANDLE WINAPI EvtOpenPublisherEnum(HANDLE session, DWORD flags) { return EvtOpenChannelEnum(session, flags); }

static BOOL enum_end(HANDLE e, DWORD *used)
{
    if (!obj_of(e, H_ENUM)) return FALSE;
    if (used) *used = 0;
    SetLastError(ERROR_NO_MORE_ITEMS_);
    return FALSE;
}
WEVTAPI BOOL WINAPI EvtNextChannelPath(HANDLE e, DWORD size, LPWSTR buf, DWORD *used)
{
    (void)size; (void)buf;
    return enum_end(e, used);
}
WEVTAPI BOOL WINAPI EvtNextPublisherId(HANDLE e, DWORD size, LPWSTR buf, DWORD *used)
{
    (void)size; (void)buf;
    return enum_end(e, used);
}

/* A channel's configuration: its properties are not set */
WEVTAPI HANDLE WINAPI EvtOpenChannelConfig(HANDLE session, LPCWSTR path, DWORD flags)
{
    if (session) { SetLastError(ERROR_NOT_SUPPORTED); return 0; }
    if (!path || flags) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    return make(H_CONFIG);
}
WEVTAPI BOOL WINAPI EvtGetChannelConfigProperty(HANDLE config, int id, DWORD flags, DWORD size, void *buf, DWORD *used)
{
    (void)id; (void)flags;
    if (!obj_of(config, H_CONFIG)) return FALSE;
    return variant(EvtVarTypeNull, 0, size, buf, used);
}

/* No publisher is registered, so none has metadata, event metadata or
 * messages to format */
WEVTAPI HANDLE WINAPI EvtOpenPublisherMetadata(HANDLE session, LPCWSTR id, LPCWSTR file, DWORD locale, DWORD flags)
{
    (void)id; (void)file; (void)locale; (void)flags;
    SetLastError(session ? ERROR_NOT_SUPPORTED : ERROR_EVT_PUBLISHER_METADATA_NOT_FOUND);
    return 0;
}
WEVTAPI BOOL WINAPI EvtGetPublisherMetadataProperty(HANDLE pub, int id, DWORD flags, DWORD size, void *buf, DWORD *used)
{
    (void)pub; (void)id; (void)flags; (void)size; (void)buf;
    if (used) *used = 0;
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}
WEVTAPI HANDLE WINAPI EvtOpenEventMetadataEnum(HANDLE pub, DWORD flags)
{
    (void)pub; (void)flags;
    SetLastError(ERROR_INVALID_HANDLE);
    return 0;
}
WEVTAPI HANDLE WINAPI EvtNextEventMetadata(HANDLE e, DWORD flags)
{
    (void)e; (void)flags;
    SetLastError(ERROR_INVALID_HANDLE);
    return 0;
}
WEVTAPI BOOL WINAPI EvtGetEventMetadataProperty(HANDLE meta, int id, DWORD flags, DWORD size, void *buf, DWORD *used)
{
    return EvtGetPublisherMetadataProperty(meta, id, flags, size, buf, used);
}
WEVTAPI BOOL WINAPI EvtGetObjectArraySize(HANDLE array, DWORD *n)
{
    (void)array;
    if (n) *n = 0;
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}
WEVTAPI BOOL WINAPI EvtGetObjectArrayProperty(HANDLE array, DWORD id, DWORD index, DWORD flags, DWORD size, void *buf, DWORD *used)
{
    (void)array; (void)id; (void)index; (void)flags; (void)size; (void)buf;
    if (used) *used = 0;
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}
WEVTAPI BOOL WINAPI EvtFormatMessage(HANDLE pub, HANDLE event, DWORD id, DWORD n, const void *values, DWORD flags,
                                     DWORD size, LPWSTR buf, DWORD *used)
{
    (void)pub; (void)event; (void)id; (void)n; (void)values; (void)flags; (void)size; (void)buf;
    if (used) *used = 0;
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}
