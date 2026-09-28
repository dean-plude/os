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
    char        name[32];
    WCHAR       wname[32], wpath[96];
    LDR_DATA_TABLE_ENTRY entry;
} Module;

#define MAX_MODULES 32
static Module g_mod[MAX_MODULES];
static int    g_nmod;                  /* modules registered with ntdll */
static int    g_ntls;                  /* static TLS slots assigned */
static PEB_LDR_DATA g_ldr;
static volatile long g_ldr_lock;
static int    g_process_ready;

static void llock(void)   { while (__atomic_exchange_n(&g_ldr_lock, 1, __ATOMIC_ACQUIRE)) __builtin_ia32_pause(); }
static void lunlock(void) { __atomic_store_n(&g_ldr_lock, 0, __ATOMIC_RELEASE); }

static BYTE *teb(void)         { BYTE *t; __asm__("movq %%gs:0x30, %0" : "=r"(t)); return t; }
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

PVOID LdrNovaGetModuleA(const char *name)
{
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

/* Allocate this thread's TLS blocks and the pointer array (TEB[0x58]). */
static int setup_thread_tls(void)
{
    if (!g_ntls) return 1;
    void **arr = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)g_ntls * sizeof(void *));
    if (!arr) return 0;
    for (int i = 0; i < g_nmod; i++) {
        Module *m = &g_mod[i];
        if (m->tls_slot < 0) continue;
        SIZE_T sz = m->tls_rawsize + m->tls_zerofill;
        BYTE *blk = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sz ? sz : 1);
        if (!blk) return 0;
        if (m->tls_rawsize) memcpy(blk, m->tls_raw, m->tls_rawsize);
        arr[m->tls_slot] = blk;
    }
    *(void **)(teb() + TEB_TLS_POINTER) = arr;
    return 1;
}

static void free_thread_tls(void)
{
    void **arr = tls_pointer();
    if (!arr) return;
    for (int i = 0; i < g_ntls; i++) if (arr[i]) RtlFreeHeap(RtlGetProcessHeap(), 0, arr[i]);
    RtlFreeHeap(RtlGetProcessHeap(), 0, arr);
    *(void **)(teb() + TEB_TLS_POINTER) = 0;
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
        for (int j = 0; j < 31 && k->Name[j]; j++) m->name[j] = k->Name[j];
        wcopy(m->wname, k->Name, 32);
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
        link(&g_ldr.InLoadOrderModuleList, &e->InLoadOrderLinks);
        link(&g_ldr.InMemoryOrderModuleList, &e->InMemoryOrderLinks);
        link(&g_ldr.InInitializationOrderModuleList, &e->InInitializationOrderLinks);
        register_tls(m);
        g_nmod++;
    }
    return first;
}

/* Run PROCESS_ATTACH for modules [first, g_nmod) that haven't run it.
 * Returns FALSE if a DLL refused to load. */
static BOOL attach_new_modules(int first)
{
    for (int i = first; i < g_nmod; i++) {
        Module *m = &g_mod[i];
        if (m->attached || !m->is_dll) { m->attached = TRUE; continue; }
        run_tls_callbacks(m, DLL_PROCESS_ATTACH);
        if (!call_dllmain(m, DLL_PROCESS_ATTACH)) return FALSE;
        m->attached = TRUE;
        m->entry.Flags |= LDRP_PROCESS_ATTACH_CALLED;
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
    RtlNovaInitExceptions();
    int first = absorb_new_modules();
    setup_thread_tls();                              /* first thread's TLS before any DllMain */
    attach_new_modules(first);
    g_process_ready = 1;
}

/* -----------------------------------------------------------------------
 * Dynamic loading
 * ----------------------------------------------------------------------- */
NTSTATUS NTAPI LdrNovaLoadDllA(const char *name, PVOID *base)
{
    llock();
    void *existing = LdrNovaGetModuleA(name);
    if (existing) { lunlock(); if (base) *base = existing; return STATUS_SUCCESS; }
    PVOID b = 0;
    NTSTATUS s = NtNovaLoadDll(name, (ULONG)strlen(name), &b);
    if (NT_SUCCESS(s)) {
        int first = absorb_new_modules();
        setup_thread_tls();                          /* refresh this thread's TLS array */
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
        if (m->is_dll && m->attached && !m->no_thread_calls) {
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
        if (m->is_dll && m->attached) call_dllmain(m, DLL_PROCESS_DETACH);
    }
}

/* Per-thread attach: TLS block + THREAD_ATTACH callbacks (not the first
 * thread, which the process init already covered). */
static void thread_attach(void)
{
    setup_thread_tls();
    for (int i = 0; i < g_nmod; i++) {
        Module *m = &g_mod[i];
        if (m->is_dll && m->attached && !m->no_thread_calls) {
            run_tls_callbacks(m, DLL_THREAD_ATTACH);
            call_dllmain(m, DLL_THREAD_ATTACH);
        }
    }
}

/* -----------------------------------------------------------------------
 * Thread entry
 * ----------------------------------------------------------------------- */
extern LONG nova_top_level_filter(PEXCEPTION_POINTERS info);   /* ntdll_exc.c */

static DWORD run_start(PUSER_THREAD_START_ROUTINE start, PVOID arg)
{
    DWORD ret = 0;
    __try {
        ret = start(arg);
    } __except (nova_top_level_filter(GetExceptionInformation())) {
        RtlExitUserProcess(GetExceptionCode());
    }
    return ret;
}

__declspec(dllexport) void NTAPI RtlUserThreadStart(PUSER_THREAD_START_ROUTINE start, PVOID arg)
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
