#define NOVA_BUILD_NTDLL
/*
 * ntdll_ldr.c — the user-mode loader: thread startup, DllMain and TLS
 *
 * The kernel maps the executable and its DLLs and lists them, in
 * dependency order, at NOVA_LDR_INFO_ADDRESS.  This code builds the PEB
 * loader list, sets up static thread-local storage, and calls each
 * module's TLS callbacks and DllMain — the initialization the kernel does
 * not do.  LoadLibrary reaches NtNovaLoadDll, which maps more modules and
 * appends them to the same list; we then initialize the new ones.
 *
 * Every thread starts here at RtlUserThreadStart(start, arg): the first
 * thread runs process initialization, every thread runs the thread-attach
 * callbacks, and then start(arg) runs under the top-level SEH frame.
 */

#include <winternl.h>
#include <winnt.h>

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);

extern void RtlNovaInitExceptions(void);       /* ntdll_exc.c */
extern void RtlNovaInitProcess(void);          /* ntdll_rtl.c */

/* -----------------------------------------------------------------------
 * Module registry (mirrors the kernel's list, plus per-module TLS state)
 * ----------------------------------------------------------------------- */
typedef BOOL (__stdcall *DllMainFn)(PVOID inst, DWORD reason, PVOID reserved);

typedef struct {
    void       *base;
    ULONG       size;
    ULONG       entry_rva;
    BOOL        is_dll;
    BOOL        attached;              /* DLL_PROCESS_ATTACH was called */
    BOOL        no_thread_calls;       /* LdrDisableThreadCalloutsForDll */
    int         tls_slot;             /* static TLS index, or -1 */
    const BYTE *tls_raw;              /* template */
    SIZE_T      tls_rawsize, tls_zerofill;
    char        name[64];
    WCHAR       wname[64], wpath[96];
    LDR_DATA_TABLE_ENTRY entry;
} Module;

#define MAX_MODULES 64
static Module g_mod[MAX_MODULES];
static int    g_nmod;                  /* modules registered with ntdll */
static int    g_ntls;                  /* static TLS slots assigned */
static PEB_LDR_DATA g_ldr;
static volatile long g_ldr_lock;
static int    g_process_ready;

static BYTE *teb(void)         { return NtCurrentTebBytes(); }

/* The loader lock: recursive, as on Windows, since a DllMain or TLS
 * callback may itself load a library (LoadLibrary under the lock) */
static void *volatile g_ldr_owner;
static int g_ldr_depth;
static void llock(void)
{
    void *me = teb();
    if (g_ldr_owner == me) { g_ldr_depth++; return; }
    for (int spins = 0; __atomic_exchange_n(&g_ldr_lock, 1, __ATOMIC_ACQUIRE); spins++) {
        if (spins < 64) __builtin_ia32_pause(); else NtYieldExecution();
    }
    g_ldr_owner = me;
    g_ldr_depth = 1;
}
static void lunlock(void)
{
    if (--g_ldr_depth) return;
    g_ldr_owner = 0;
    __atomic_store_n(&g_ldr_lock, 0, __ATOMIC_RELEASE);
}
static void *tls_pointer(void) { return *(void **)(teb() + TEB_TLS_POINTER); }

static void wcopy(WCHAR *d, const char *s, int cap)
{
    int i = 0;
    for (; s[i] && i < cap - 1; i++) d[i] = (WCHAR)(BYTE)s[i];
    d[i] = 0;
}

static IMAGE_NT_HEADERS *nt_of(void *base)
{
    IMAGE_DOS_HEADER *dos = base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE ? nt : 0;
}

static IMAGE_DATA_DIRECTORY *dir_of(void *base, int which)
{
    IMAGE_NT_HEADERS *nt = nt_of(base);
    if (!nt || (ULONG)which >= nt->OptionalHeader.NumberOfRvaAndSizes) return 0;
    return &nt->OptionalHeader.DataDirectory[which];
}

/* A path names a module by its full path; a bare name by its file name */
static int path_eq(const WCHAR *a, const char *b)
{
    for (;; a++, b++) {
        unsigned x = *a, y = (unsigned char)*b;
        if (x == '/') x = '\\';
        if (y == '/') y = '\\';
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

/* An API set name stands for the DLL that implements it (the kernel's
 * loader maps them the same way, see map_api_set in um.c), so
 * GetModuleHandle("api-ms-win-core-synch-l1-2-0") finds kernel32 as on
 * Windows.  Rust's standard library looks up WaitOnAddress that way. */
static const char *api_set_host(const char *name)
{
    static const struct { const char *prefix, *dll; } sets[] = {
        { "api-ms-win-crt-",        "ucrtbase.dll" },
        { "api-ms-win-core-com-",   "ole32.dll" },
        { "api-ms-win-core-winrt-", "ole32.dll" },
        { "combase",                "ole32.dll" },
        { "api-ms-win-core-",       "kernel32.dll" },
        { "api-ms-win-security-",   "advapi32.dll" },
        { "api-ms-win-eventing-",   "advapi32.dll" },
        { "api-ms-win-shell-",      "shell32.dll" },
        { "api-ms-win-shcore-",     "shlwapi.dll" },
        { "ext-ms-win-",            "kernel32.dll" },
        { "kernelbase",             "kernel32.dll" },
        { "api-ms-win-",            "kernel32.dll" },
    };
    for (unsigned i = 0; i < sizeof(sets) / sizeof(sets[0]); i++) {
        const char *p = sets[i].prefix, *n = name;
        while (*p && (*n | 0x20) == *p) p++, n++;
        if (!*p) return sets[i].dll;
    }
    return 0;
}

PVOID LdrNovaGetModuleA(const char *name)
{
    const char *host = api_set_host(name);
    if (host) name = host;
    int has_dir = 0;
    for (const char *c = name; *c; c++) if (*c == '\\' || *c == '/') has_dir = 1;
    if (has_dir) {
        for (int i = 0; i < g_nmod; i++) if (path_eq(g_mod[i].wpath, name)) return g_mod[i].base;
        return 0;
    }
    for (int i = 0; i < g_nmod; i++) {
        const char *a = g_mod[i].name, *b = name;
        int eq = 1;
        while (*a && *b) {
            char x = *a, y = *b;
            if (x >= 'A' && x <= 'Z') x += 32;
            if (y >= 'A' && y <= 'Z') y += 32;
            if (x != y) { eq = 0; break; }
            a++; b++;
        }
        /* match with or without a trailing ".dll" on the query */
        if (eq && (!*a || !strcmp(a, ".dll")) && (!*b || !strcmp(b, ".dll"))) return g_mod[i].base;
    }
    return 0;
}

PLDR_DATA_TABLE_ENTRY LdrNovaFindEntry(PVOID address)
{
    for (int i = 0; i < g_nmod; i++)
        if ((BYTE *)address >= (BYTE *)g_mod[i].base && (BYTE *)address < (BYTE *)g_mod[i].base + g_mod[i].size)
            return &g_mod[i].entry;
    return 0;
}

/* -----------------------------------------------------------------------
 * Static TLS
 * ----------------------------------------------------------------------- */
static void register_tls(Module *m)
{
    IMAGE_DATA_DIRECTORY *d = dir_of(m->base, IMAGE_DIRECTORY_ENTRY_TLS);
    m->tls_slot = -1;
    if (!d || !d->VirtualAddress || d->Size < sizeof(IMAGE_TLS_DIRECTORY)) return;
    IMAGE_TLS_DIRECTORY *tls = (IMAGE_TLS_DIRECTORY *)((BYTE *)m->base + d->VirtualAddress);
    m->tls_raw = (const BYTE *)tls->StartAddressOfRawData;
    m->tls_rawsize = tls->EndAddressOfRawData - tls->StartAddressOfRawData;
    m->tls_zerofill = tls->SizeOfZeroFill;
    m->tls_slot = g_ntls++;
    if (tls->AddressOfIndex) *(ULONG *)tls->AddressOfIndex = (ULONG)m->tls_slot;   /* _tls_index */
}

/* Each thread's TLS pointer array (TEB[0x58]) is preceded by its length,
 * so a DLL loaded later (LoadLibrary) grows every live thread's array with
 * a fresh block for its slot, keeping the blocks already there — as
 * Windows does.  The threads with arrays are listed for that. */
#define MAX_TLS_THREADS 2048
static BYTE *g_tls_threads[MAX_TLS_THREADS];

static void *tls_template_block(int slot)
{
    for (int i = 0; i < g_nmod; i++) {
        Module *m = &g_mod[i];
        if (m->tls_slot != slot) continue;
        SIZE_T sz = m->tls_rawsize + m->tls_zerofill;
        BYTE *blk = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sz ? sz : 1);
        if (blk && m->tls_rawsize) memcpy(blk, m->tls_raw, m->tls_rawsize);
        return blk;
    }
    return 0;
}

/* Give the thread of @t (its TEB) a block for every slot (loader lock held) */
static int grow_tls(BYTE *t)
{
    void **old = *(void ***)(t + TEB_TLS_POINTER);
    ULONG_PTR had = old ? (ULONG_PTR)old[-1] : 0;
    if ((int)had >= g_ntls) return 1;
    void **arr = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, ((SIZE_T)g_ntls + 1) * sizeof(void *));
    if (!arr) return 0;
    arr++;
    for (ULONG_PTR i = 0; i < had; i++) arr[i] = old[i];
    for (int i = (int)had; i < g_ntls; i++) arr[i] = tls_template_block(i);
    arr[-1] = (void *)(ULONG_PTR)g_ntls;
    __atomic_store_n((void ***)(t + TEB_TLS_POINTER), arr, __ATOMIC_RELEASE);
    /* (the old array is left: the thread may be reading it right now) */
    return 1;
}

/* This thread's TLS: listed, and a block for every module's slot */
static int setup_thread_tls(void)
{
    llock();
    BYTE *me = teb();
    int listed = 0, fr = -1;
    for (int i = 0; i < MAX_TLS_THREADS; i++) {
        if (g_tls_threads[i] == me) { listed = 1; break; }
        if (!g_tls_threads[i] && fr < 0) fr = i;
    }
    if (!listed && fr >= 0) g_tls_threads[fr] = me;
    int ok = g_ntls ? grow_tls(me) : 1;
    lunlock();
    return ok;
}

/* After a LoadLibrary: every listed thread gets the new modules' blocks */
static void grow_all_tls(void)
{
    for (int i = 0; i < MAX_TLS_THREADS; i++)
        if (g_tls_threads[i]) grow_tls(g_tls_threads[i]);
}

static void free_thread_tls(void)
{
    llock();
    BYTE *me = teb();
    for (int i = 0; i < MAX_TLS_THREADS; i++) if (g_tls_threads[i] == me) g_tls_threads[i] = 0;
    void **arr = tls_pointer();
    if (arr) {
        ULONG_PTR n = (ULONG_PTR)arr[-1];
        for (ULONG_PTR i = 0; i < n; i++) if (arr[i]) RtlFreeHeap(RtlGetProcessHeap(), 0, arr[i]);
        *(void **)(me + TEB_TLS_POINTER) = 0;
        RtlFreeHeap(RtlGetProcessHeap(), 0, arr - 1);
    }
    lunlock();
}

/* -----------------------------------------------------------------------
 * TLS callbacks and DllMain
 * ----------------------------------------------------------------------- */
static void run_tls_callbacks(Module *m, DWORD reason)
{
    IMAGE_DATA_DIRECTORY *d = dir_of(m->base, IMAGE_DIRECTORY_ENTRY_TLS);
    if (!d || !d->VirtualAddress) return;
    IMAGE_TLS_DIRECTORY *tls = (IMAGE_TLS_DIRECTORY *)((BYTE *)m->base + d->VirtualAddress);
    PIMAGE_TLS_CALLBACK *cb = (PIMAGE_TLS_CALLBACK *)tls->AddressOfCallBacks;
    if (!cb) return;
    for (; *cb; cb++) (*cb)(m->base, reason, 0);
}

static BOOL call_dllmain(Module *m, DWORD reason)
{
    if (!m->is_dll || !m->entry_rva) return TRUE;
    DllMainFn fn = (DllMainFn)((BYTE *)m->base + m->entry_rva);
    return fn(m->base, reason, (PVOID)(ULONG_PTR)1);          /* reserved=1: static load */
}

/* -----------------------------------------------------------------------
 * Building the loader list from the kernel's info
 * ----------------------------------------------------------------------- */
static void link_first(LIST_ENTRY *head, LIST_ENTRY *e)
{
    e->Flink = head->Flink;
    e->Blink = head;
    head->Flink->Blink = e;
    head->Flink = e;
}

static void link(LIST_ENTRY *head, LIST_ENTRY *e)
{
    e->Blink = head->Blink;
    e->Flink = head;
    head->Blink->Flink = e;
    head->Blink = e;
}

/* Register modules the kernel has added since g_nmod; assign TLS slots. */
static int absorb_new_modules(void)
{
    NOVA_LDR_INFO *info = NOVA_LDR_INFO_ADDRESS;
    ULONG count = info->Count;
    if (count > MAX_MODULES) count = MAX_MODULES;
    int first = g_nmod;
    for (ULONG i = g_nmod; i < count; i++) {
        NOVA_LDR_MODULE *k = &info->Modules[i];
        Module *m = &g_mod[g_nmod];
        m->base = (void *)(ULONG_PTR)k->Base;
        m->size = (ULONG)k->Size;
        m->entry_rva = k->EntryRva;
        m->is_dll = (k->Flags & 1) != 0;
        m->tls_slot = -1;
        for (int j = 0; j < 63 && k->Name[j]; j++) m->name[j] = k->Name[j];
        wcopy(m->wname, k->Name, 64);
        wcopy(m->wpath, k->Path, 96);
        LDR_DATA_TABLE_ENTRY *e = &m->entry;
        e->DllBase = m->base;
        e->EntryPoint = m->entry_rva ? (BYTE *)m->base + m->entry_rva : 0;
        e->SizeOfImage = m->size;
        e->BaseDllName.Buffer = m->wname;
        e->BaseDllName.Length = (USHORT)(strlen(m->name) * 2);
        e->BaseDllName.MaximumLength = e->BaseDllName.Length + 2;
        e->FullDllName.Buffer = m->wpath;
        { int n = 0; while (m->wpath[n]) n++; e->FullDllName.Length = (USHORT)(n * 2); e->FullDllName.MaximumLength = (USHORT)(n * 2 + 2); }
        e->Flags = m->is_dll ? LDRP_IMAGE_DLL : 0;
        e->LoadCount = 1;
        if (m->is_dll) {
            link(&g_ldr.InLoadOrderModuleList, &e->InLoadOrderLinks);
            link(&g_ldr.InMemoryOrderModuleList, &e->InMemoryOrderLinks);
            link(&g_ldr.InInitializationOrderModuleList, &e->InInitializationOrderLinks);
        } else {                                     /* as on Windows: the program heads the */
            link_first(&g_ldr.InLoadOrderModuleList, &e->InLoadOrderLinks);     /* load and memory */
            link_first(&g_ldr.InMemoryOrderModuleList, &e->InMemoryOrderLinks); /* lists, and is not */
            e->InInitializationOrderLinks.Flink = e->InInitializationOrderLinks.Blink =   /* initialized */
                &e->InInitializationOrderLinks;
        }
        g_nmod++;
    }
    /* TLS slots: the program's first, as on Windows: an .exe's code may
     * take its _tls_index to be 0 (MSVC's thread-safe statics read slot 0
     * directly), whatever DLLs with TLS the kernel listed before it */
    for (int pass = 0; pass < 2; pass++)
        for (int i = first; i < g_nmod; i++)
            if (g_mod[i].is_dll == pass) register_tls(&g_mod[i]);
    return first;
}

/* Run PROCESS_ATTACH for modules [first, g_nmod) that haven't run it.
 * Returns FALSE if a DLL refused to load. */
static BOOL attach_new_modules(int first)
{
    for (int i = first; i < g_nmod; i++) {
        Module *m = &g_mod[i];
        if (m->attached || !m->is_dll) continue;
        m->attached = TRUE;                          /* (a nested load must not run it again) */
        run_tls_callbacks(m, DLL_PROCESS_ATTACH);
        if (!call_dllmain(m, DLL_PROCESS_ATTACH)) return FALSE;
        m->entry.Flags |= LDRP_PROCESS_ATTACH_CALLED;
    }
    /* the program's own TLS callbacks run once its DLLs are initialized,
     * before its entry point (MinGW programs set up thread-local data and
     * winpthreads' cleanup there) */
    for (int i = first; i < g_nmod; i++) {
        Module *m = &g_mod[i];
        if (m->attached) continue;
        m->attached = TRUE;
        run_tls_callbacks(m, DLL_PROCESS_ATTACH);
    }
    return TRUE;
}

static void ldr_init_process(void)
{
    PPEB peb = RtlGetCurrentPeb();
    g_ldr.Length = sizeof(g_ldr);
    g_ldr.Initialized = TRUE;
    g_ldr.InLoadOrderModuleList.Flink = g_ldr.InLoadOrderModuleList.Blink = &g_ldr.InLoadOrderModuleList;
    g_ldr.InMemoryOrderModuleList.Flink = g_ldr.InMemoryOrderModuleList.Blink = &g_ldr.InMemoryOrderModuleList;
    g_ldr.InInitializationOrderModuleList.Flink = g_ldr.InInitializationOrderModuleList.Blink = &g_ldr.InInitializationOrderModuleList;
    peb->Ldr = &g_ldr;
    RtlNovaInitProcess();
    RtlNovaInitExceptions();
    int first = absorb_new_modules();
    setup_thread_tls();                              /* first thread's TLS before any DllMain */
    attach_new_modules(first);
    g_process_ready = 1;
    NtTestAlert();                                   /* APCs the DLLs queued to this thread (as Windows) */
}

/* -----------------------------------------------------------------------
 * Dynamic loading
 * ----------------------------------------------------------------------- */
NTSTATUS NTAPI LdrNovaLoadDllA(const char *name, PVOID *base) { return LdrNovaLoadDllExA(name, 0, base); }

/* @flags: LoadLibraryEx's (the kernel maps AS_DATAFILE / AS_IMAGE_RESOURCE
 * modules as data) */
NTSTATUS NTAPI LdrNovaLoadDllExA(const char *name, ULONG flags, PVOID *base)
{
    llock();
    void *existing = LdrNovaGetModuleA(name);
    if (existing) { lunlock(); if (base) *base = existing; return STATUS_SUCCESS; }
    PVOID b = 0;
    NTSTATUS s = NtNovaLoadDll(name, (ULONG)strlen(name), &b, flags);
    if (NT_SUCCESS(s)) {
        int first = absorb_new_modules();
        setup_thread_tls();
        grow_all_tls();                              /* the new modules' TLS, in every thread */
        if (!attach_new_modules(first)) s = STATUS_DLL_INIT_FAILED;
        if (base) *base = b;
    }
    lunlock();
    return s;
}

NTSTATUS NTAPI LdrLoadDll(const WCHAR *path, PULONG flags, PUNICODE_STRING name, PVOID *base)
{
    (void)path; (void)flags;
    char n[128];
    int i = 0;
    if (name && name->Buffer) for (; i < 127 && i < name->Length / 2; i++) n[i] = (char)name->Buffer[i];
    n[i] = 0;
    return LdrNovaLoadDllA(n, base);
}

NTSTATUS NTAPI LdrGetDllHandle(const WCHAR *path, PULONG flags, PUNICODE_STRING name, PVOID *base)
{
    (void)path; (void)flags;
    char n[128];
    int i = 0;
    if (name && name->Buffer) for (; i < 127 && i < name->Length / 2; i++) n[i] = (char)name->Buffer[i];
    n[i] = 0;
    void *b = LdrNovaGetModuleA(n);
    if (base) *base = b;
    return b ? STATUS_SUCCESS : STATUS_DLL_NOT_FOUND;
}

NTSTATUS NTAPI LdrDisableThreadCalloutsForDll(PVOID base)
{
    for (int i = 0; i < g_nmod; i++) if (g_mod[i].base == base) { g_mod[i].no_thread_calls = TRUE; return STATUS_SUCCESS; }
    return STATUS_DLL_NOT_FOUND;
}

/* Export lookup shared with kernel32's GetProcAddress */
NTSTATUS NTAPI LdrGetProcedureAddress(PVOID base, const char *name, ULONG ordinal, PVOID *addr)
{
    IMAGE_DATA_DIRECTORY *d = dir_of(base, IMAGE_DIRECTORY_ENTRY_EXPORT);
    if (!d || !d->VirtualAddress) return STATUS_PROCEDURE_NOT_FOUND;
    BYTE *b = base;
    IMAGE_EXPORT_DIRECTORY *ed = (IMAGE_EXPORT_DIRECTORY *)(b + d->VirtualAddress);
    DWORD *funcs = (DWORD *)(b + ed->AddressOfFunctions);
    DWORD rva = 0;
    if (!name) {
        DWORD idx = ordinal - ed->Base;
        if (idx < ed->NumberOfFunctions) rva = funcs[idx];
    } else {
        DWORD *names = (DWORD *)(b + ed->AddressOfNames);
        WORD *ords = (WORD *)(b + ed->AddressOfNameOrdinals);
        for (DWORD i = 0; i < ed->NumberOfNames; i++)
            if (!strcmp((const char *)(b + names[i]), name)) { rva = funcs[ords[i]]; break; }
    }
    if (!rva || (rva >= d->VirtualAddress && rva < d->VirtualAddress + d->Size)) return STATUS_PROCEDURE_NOT_FOUND;
    if (addr) *addr = b + rva;
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * Thread and process teardown callbacks (called from RtlExit*)
 * ----------------------------------------------------------------------- */
void nova_run_thread_detach(void)
{
    if (!g_process_ready) return;
    for (int i = g_nmod - 1; i >= 0; i--) {
        Module *m = &g_mod[i];
        if (m->attached && !m->no_thread_calls) {
            run_tls_callbacks(m, DLL_THREAD_DETACH);
            call_dllmain(m, DLL_THREAD_DETACH);
        }
    }
    free_thread_tls();
}

void nova_run_process_detach(void)
{
    if (!g_process_ready) return;
    for (int i = g_nmod - 1; i >= 0; i--) {
        Module *m = &g_mod[i];
        if (!m->attached) continue;
        if (!m->is_dll) run_tls_callbacks(m, DLL_PROCESS_DETACH);
        else call_dllmain(m, DLL_PROCESS_DETACH);
    }
}

/* Per-thread attach: TLS block + THREAD_ATTACH callbacks (not the first
 * thread, which the process init already covered). */
static void thread_attach(void)
{
    setup_thread_tls();
    for (int i = 0; i < g_nmod; i++) {
        Module *m = &g_mod[i];
        if (m->attached && !m->no_thread_calls) {
            run_tls_callbacks(m, DLL_THREAD_ATTACH);
            call_dllmain(m, DLL_THREAD_ATTACH);
        }
    }
}

/* -----------------------------------------------------------------------
 * Thread entry
 * ----------------------------------------------------------------------- */
extern LONG nova_top_level_filter(PEXCEPTION_POINTERS info);   /* ntdll_exc.c */

#ifndef _WIN64
DWORD nova_call_start(PVOID fn, PVOID arg) __asm__("nova_call_start");     /* exc_x86.h */
#endif

static DWORD run_start(PUSER_THREAD_START_ROUTINE start, PVOID arg)
{
    DWORD ret = 0;
    __try {
#ifdef _WIN64
        ret = start(arg);
#else
        ret = nova_call_start((PVOID)start, arg);   /* the stack survives a start routine that isn't stdcall */
#endif
    } __except (nova_top_level_filter(GetExceptionInformation())) {
        RtlExitUserProcess(GetExceptionCode());
    }
    return ret;
}

#ifdef _WIN64
__declspec(dllexport) void NTAPI RtlUserThreadStart(PUSER_THREAD_START_ROUTINE start, PVOID arg)
#else
/* 32-bit: the kernel starts threads with ECX = start, EDX = arg */
__asm__(".text\n.globl _RtlUserThreadStart\n_RtlUserThreadStart:\n\t"
        "pushl %edx\n\tpushl %ecx\n\tpushl $0\n\tjmp nova_thread_start32\n"
        ".section .drectve,\"yn\"\n\t.ascii \" /EXPORT:_RtlUserThreadStart\"\n\t.text\n");
void NTAPI nova_thread_start32(PUSER_THREAD_START_ROUTINE start, PVOID arg) __asm__("nova_thread_start32");
void NTAPI nova_thread_start32(PUSER_THREAD_START_ROUTINE start, PVOID arg)
#endif
{
    int first;
    llock();
    first = !g_process_ready;
    if (first) ldr_init_process();
    else thread_attach();
    lunlock();

    DWORD ret = run_start(start, arg);
    RtlExitUserThread((NTSTATUS)ret);
}

/* -----------------------------------------------------------------------
 * Thread creation helper used by kernel32!CreateThread
 * ----------------------------------------------------------------------- */
NTSTATUS NTAPI RtlNovaCreateThread(PUSER_THREAD_START_ROUTINE start, PVOID arg, SIZE_T stack,
                                   BOOL suspended, PHANDLE h, PULONG tid)
{
    HANDLE th = 0;
    NTSTATUS s = NtCreateThreadEx(&th, 0, 0, NtCurrentProcess(), (PVOID)start, arg,
                                  suspended ? 1 : 0, 0, 0, stack, 0);
    if (!NT_SUCCESS(s)) return s;
    if (h) *h = th;
    if (tid) {
        THREAD_BASIC_INFORMATION tbi;
        if (NT_SUCCESS(NtQueryInformationThread(th, 0, &tbi, sizeof(tbi), 0)))
            *tid = (ULONG)(ULONG_PTR)tbi.ClientId.UniqueThread;
    }
    return STATUS_SUCCESS;
}

VOID NTAPI LdrNovaZeroTlsCell(ULONG index)
{
    void **arr = tls_pointer();
    (void)arr; (void)index;
}
