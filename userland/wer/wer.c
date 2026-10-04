/*
 * wer.dll — Windows Error Reporting's report API.  NovaOS has no error
 * reporting service and sends reports nowhere, as on a Windows machine
 * whose reporting is turned off: a report can be made, filled in and
 * closed, and submitting it answers WerDisabled.  Programs that report
 * their own crashes (Chromium's setup.exe, which delay-loads this DLL;
 * Microsoft Edge Update, which loads it) go on as they would there.
 * Written for NovaOS from the documented API (no reporting code exists
 * under a licence NovaOS reuses).
 */
#include <windows.h>

#define WERAPI __declspec(dllexport)

#define WER_MAX_PARAMS         10               /* WER_P0 .. WER_P9 */
#define WER_MAX_STRING         256
#define WER_MAX_FILES          512
#define WerDisabled            5                /* WER_SUBMIT_RESULT */
#define WER_REPORT_MAGIC       0x52455257u      /* "WRER" */

typedef struct {
    DWORD magic;
    int   type;                                 /* WER_REPORT_TYPE: 0 non-critical .. 3 application wait */
    WCHAR event[WER_MAX_STRING];
    DWORD nparams, nfiles, ndumps;
    WCHAR param_name[WER_MAX_PARAMS][WER_MAX_STRING];
    WCHAR param[WER_MAX_PARAMS][WER_MAX_STRING];
} WerReport;

static WerReport *report_of(HANDLE h)
{
    WerReport *r = (WerReport *)h;
    return r && r->magic == WER_REPORT_MAGIC ? r : NULL;
}

static void copy_str(WCHAR *to, PCWSTR from)
{
    int i = 0;
    if (from) for (; from[i] && i < WER_MAX_STRING - 1; i++) to[i] = from[i];
    to[i] = 0;
}

/* WerReportCreate(PCWSTR EventType, WER_REPORT_TYPE, PWER_REPORT_INFORMATION, HREPORT *) */
WERAPI HRESULT WINAPI WerReportCreate(PCWSTR event, int type, PVOID info, HANDLE *out)
{
    (void)info;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!event || !event[0] || lstrlenW(event) >= WER_MAX_STRING || type < 0 || type > 3) return E_INVALIDARG;
    WerReport *r = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*r));
    if (!r) return E_OUTOFMEMORY;
    r->magic = WER_REPORT_MAGIC;
    r->type = type;
    copy_str(r->event, event);
    *out = r;
    return S_OK;
}

WERAPI HRESULT WINAPI WerReportCloseHandle(HANDLE h)
{
    WerReport *r = report_of(h);
    if (!r) return E_INVALIDARG;
    r->magic = 0;
    HeapFree(GetProcessHeap(), 0, r);
    return S_OK;
}

/* WerReportSetParameter(HREPORT, DWORD dwparamID (WER_P0..WER_P9), PCWSTR name, PCWSTR value) */
WERAPI HRESULT WINAPI WerReportSetParameter(HANDLE h, DWORD id, PCWSTR name, PCWSTR value)
{
    WerReport *r = report_of(h);
    if (!r || id >= WER_MAX_PARAMS || !value) return E_INVALIDARG;
    if (lstrlenW(value) >= WER_MAX_STRING || (name && lstrlenW(name) >= WER_MAX_STRING)) return E_INVALIDARG;
    copy_str(r->param_name[id], name);
    copy_str(r->param[id], value);
    if (id + 1 > r->nparams) r->nparams = id + 1;
    return S_OK;
}

/* WerReportAddFile(HREPORT, PCWSTR path, WER_FILE_TYPE, DWORD flags): the
 * file would go with the report; it is left where it is */
WERAPI HRESULT WINAPI WerReportAddFile(HANDLE h, PCWSTR path, int type, DWORD flags)
{
    WerReport *r = report_of(h);
    if (!r || !path || !path[0] || type < 1 || type > 8) return E_INVALIDARG;
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return HRESULT_FROM_WIN32(GetLastError());
    if (r->nfiles >= WER_MAX_FILES) return HRESULT_FROM_WIN32(ERROR_TOO_MANY_OPEN_FILES);
    r->nfiles++;
    (void)flags;
    return S_OK;
}

/* WerReportAddDump(HREPORT, HANDLE process, HANDLE thread, WER_DUMP_TYPE,
 * PWER_EXCEPTION_INFORMATION, PWER_DUMP_CUSTOM_OPTIONS, DWORD flags): the
 * dump would be written when the report is submitted, which never happens */
WERAPI HRESULT WINAPI WerReportAddDump(HANDLE h, HANDLE process, HANDLE thread, int type, PVOID exc, PVOID opts, DWORD flags)
{
    (void)thread; (void)exc; (void)opts; (void)flags;
    WerReport *r = report_of(h);
    if (!r || !process || type < 1 || type > 5) return E_INVALIDARG;
    r->ndumps++;
    return S_OK;
}

/* WerReportSetUIOption(HREPORT, WER_REPORT_UI, PCWSTR value) */
WERAPI HRESULT WINAPI WerReportSetUIOption(HANDLE h, int ui, PCWSTR value)
{
    WerReport *r = report_of(h);
    if (!r || ui < 0 || ui > 10 || !value) return E_INVALIDARG;
    return S_OK;
}

/* WerReportSubmit(HREPORT, WER_CONSENT, DWORD flags, PWER_SUBMIT_RESULT):
 * reporting is turned off, so nothing is sent, queued or shown */
WERAPI HRESULT WINAPI WerReportSubmit(HANDLE h, int consent, DWORD flags, int *result)
{
    (void)consent; (void)flags;
    WerReport *r = report_of(h);
    if (!r) return E_INVALIDARG;
    if (result) *result = WerDisabled;
    return S_OK;
}

/* Excluded applications are never reported; nothing is reported here */
WERAPI HRESULT WINAPI WerAddExcludedApplication(PCWSTR exe, BOOL all_users)
{
    (void)all_users;
    return exe && exe[0] ? S_OK : E_INVALIDARG;
}

WERAPI HRESULT WINAPI WerRemoveExcludedApplication(PCWSTR exe, BOOL all_users)
{
    (void)all_users;
    return exe && exe[0] ? S_OK : E_INVALIDARG;
}
