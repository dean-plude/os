/*
 * dbghelp.dll — debugging helpers.  Symbol handling starts and answers
 * that no symbols are loaded; stack walks use the unwinder (x64) and
 * module lookups the loader.  Minidumps are not written.
 */
#include <windows.h>
#include <winternl.h>

#define DBGHELPAPI __declspec(dllexport)

static DWORD g_options;
static WCHAR g_search[MAX_PATH];

DBGHELPAPI BOOL WINAPI SymInitialize(HANDLE p, LPCSTR path, BOOL invade) { (void)p; (void)path; (void)invade; return TRUE; }
DBGHELPAPI BOOL WINAPI SymInitializeW(HANDLE p, LPCWSTR path, BOOL invade) { (void)p; (void)path; (void)invade; return TRUE; }
DBGHELPAPI BOOL WINAPI SymCleanup(HANDLE p) { (void)p; return TRUE; }
DBGHELPAPI DWORD WINAPI SymGetOptions(void) { return g_options; }
DBGHELPAPI DWORD WINAPI SymSetOptions(DWORD o) { g_options = o; return o; }
DBGHELPAPI BOOL WINAPI SymSetSearchPathW(HANDLE p, LPCWSTR s)
{
    (void)p;
    int i = 0;
    for (; s && s[i] && i < MAX_PATH - 1; i++) g_search[i] = s[i];
    g_search[i] = 0;
    return TRUE;
}
DBGHELPAPI BOOL WINAPI SymGetSearchPathW(HANDLE p, LPWSTR s, DWORD n)
{
    (void)p;
    if (!s || !n) return FALSE;
    DWORD i = 0;
    for (; g_search[i] && i < n - 1; i++) s[i] = g_search[i];
    s[i] = 0;
    return TRUE;
}
DBGHELPAPI BOOL WINAPI SymFromAddr(HANDLE p, DWORD64 a, PDWORD64 disp, PVOID sym)
{
    (void)p; (void)a; (void)disp; (void)sym;
    SetLastError(ERROR_MOD_NOT_FOUND);
    return FALSE;
}
DBGHELPAPI BOOL WINAPI SymGetLineFromAddr64(HANDLE p, DWORD64 a, PDWORD disp, PVOID line)
{
    (void)p; (void)a; (void)disp; (void)line;
    SetLastError(ERROR_MOD_NOT_FOUND);
    return FALSE;
}
DBGHELPAPI PVOID WINAPI SymFunctionTableAccess64(HANDLE p, DWORD64 a)
{
    (void)p;
#ifdef _WIN64
    DWORD64 base;
    return RtlLookupFunctionEntry(a, &base, 0);
#else
    (void)a;
    return 0;
#endif
}
DBGHELPAPI DWORD64 WINAPI SymGetModuleBase64(HANDLE p, DWORD64 a)
{
    (void)p;
    PVOID base = 0;
    RtlPcToFileHeader((PVOID)(ULONG_PTR)a, &base);
    return (DWORD64)(ULONG_PTR)base;
}

/* StackWalk64: one frame further up each call (x64: the unwinder; the
 * caller's CONTEXT is updated) */
typedef struct { DWORD64 Offset; WORD Segment; int Mode; } ADDRESS64_;
typedef struct {
    ADDRESS64_ AddrPC, AddrReturn, AddrFrame, AddrStack, AddrBStore;
    PVOID FuncTableEntry;
    DWORD64 Params[4];
    BOOL Far, Virtual;
    DWORD64 Reserved[3];
    BYTE KdHelp[0x70];
} STACKFRAME64_;
DBGHELPAPI BOOL WINAPI StackWalk64(DWORD machine, HANDLE p, HANDLE t, STACKFRAME64_ *f, PVOID ctx, PVOID rd, PVOID fta, PVOID gmb, PVOID tr)
{
    (void)machine; (void)p; (void)t; (void)rd; (void)fta; (void)gmb; (void)tr;
#ifdef _WIN64
    CONTEXT *c = ctx;
    if (!f || !c) return FALSE;
    if (!f->AddrReturn.Offset && !f->AddrPC.Offset) {        /* the first frame: where the context is */
        f->AddrPC.Offset = c->Rip; f->AddrStack.Offset = c->Rsp; f->AddrFrame.Offset = c->Rbp;
        f->AddrReturn.Offset = 1;
        return c->Rip != 0;
    }
    DWORD64 base;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c->Rip, &base, 0);
    if (fn) {
        PVOID hd; DWORD64 est;
        RtlVirtualUnwind(0, base, c->Rip, fn, c, &hd, &est, 0);
    } else {                                                 /* a leaf: the return address is on top */
        c->Rip = *(DWORD64 *)c->Rsp;
        c->Rsp += 8;
    }
    if (!c->Rip) return FALSE;
    f->AddrPC.Offset = c->Rip; f->AddrStack.Offset = c->Rsp; f->AddrFrame.Offset = c->Rbp;
    return TRUE;
#else
    (void)f; (void)ctx;
    return FALSE;
#endif
}

DBGHELPAPI BOOL WINAPI MiniDumpWriteDump(HANDLE p, DWORD pid, HANDLE file, int type, PVOID e, PVOID u, PVOID cb)
{
    (void)p; (void)pid; (void)file; (void)type; (void)e; (void)u; (void)cb;
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

/* The decorated name, undecorated by not much: copied as it is */
DBGHELPAPI DWORD WINAPI UnDecorateSymbolName(LPCSTR name, LPSTR out, DWORD n, DWORD flags)
{
    (void)flags;
    if (!name || !out || !n) return 0;
    DWORD i = 0;
    for (; name[i] && i < n - 1; i++) out[i] = name[i];
    out[i] = 0;
    return i;
}
DBGHELPAPI PVOID WINAPI ImageNtHeader(PVOID base)
{
    IMAGE_DOS_HEADER *d = base;
    if (!d || d->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)base + d->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE ? nt : 0;
}
