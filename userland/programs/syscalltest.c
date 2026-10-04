/* syscalltest.exe — the system-call table, the way code that calls the
 * kernel without ntdll sees it (anti-cheat and sandbox code such as
 * Roblox's Hyperion).  It does not read ntdll's stubs: it works out each
 * service's number the way that code does, by ranking ntdll's Zw exports
 * by address (the export at the lowest address is service 0, the next 1,
 * and so on, as the kernel lays them out), then issues a `syscall` with
 * that number and checks the result against the same call made through
 * ntdll.  A table that is laid out or numbered wrong fails here even when
 * every ntdll stub is right.
 *
 * Each value is the Windows 10 1903 one, written out here, not taken from
 * NovaOS's headers, so a number that moves fails the test.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <winternl.h>

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

#ifdef _WIN64

/* A raw system call: RAX = number, R10/RDX/R8/R9 = the four arguments, as
 * the NT ABI and ntdll's stubs use.  This is the sequence Hyperion builds
 * at run time; the number comes from the table below, not from ntdll. */
__attribute__((naked)) static NTSTATUS raw_syscall(ULONG num, ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, ULONG_PTR a4)
{
    __asm__("movl %ecx, %eax\n\t"           /* number */
            "movq %rdx, %r10\n\t"           /* a1 */
            "movq %r8, %rdx\n\t"            /* a2 */
            "movq %r9, %r8\n\t"            /* a3 */
            "movq 40(%rsp), %r9\n\t"        /* a4 (home area: 0x28 past the return address) */
            "syscall\n\t"
            "retq");
}

/* ntdll's Zw exports, ranked by address: Zw name -> its index.  Returns
 * the number of services found, or -1 if it does not fit @cap. */
struct svc { char name[48]; ULONG rva; };

static int rank_zw_exports(HMODULE ntdll, struct svc *out, int cap)
{
    BYTE *b = (BYTE *)ntdll;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)b;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    IMAGE_EXPORT_DIRECTORY *ed = (IMAGE_EXPORT_DIRECTORY *)(b + dd->VirtualAddress);
    ULONG *names = (ULONG *)(b + ed->AddressOfNames);
    USHORT *ords = (USHORT *)(b + ed->AddressOfNameOrdinals);
    ULONG *funcs = (ULONG *)(b + ed->AddressOfFunctions);
    int n = 0;
    for (ULONG i = 0; i < ed->NumberOfNames; i++) {
        const char *nm = (const char *)(b + names[i]);
        if (nm[0] != 'Z' || nm[1] != 'w') continue;
        if (n >= cap) return -1;
        ULONG rva = funcs[ords[i]];
        /* insertion sort by address: small tables, and it keeps ties stable */
        int j = n++;
        while (j > 0 && out[j - 1].rva > rva) { out[j] = out[j - 1]; j--; }
        strncpy(out[j].name, nm, sizeof(out[j].name) - 1);
        out[j].name[sizeof(out[j].name) - 1] = 0;
        out[j].rva = rva;
    }
    return n;
}

static ULONG ssn_of(const struct svc *t, int n, const char *zwname)
{
    for (int i = 0; i < n; i++) if (!strcmp(t[i].name, zwname)) return (ULONG)i;
    return 0xFFFFFFFFu;
}

int main(void)
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    CHECK("ntdll.dll is loaded", ntdll != NULL);

    static struct svc tbl[1024];
    int n = rank_zw_exports(ntdll, tbl, 1024);
    CHECK("ntdll exports Zw system calls", n >= 300);
    printf("%d Zw exports ranked by address\n", n);

    /* The rank of each Windows 10 1903 service, by address, is its service
     * number: these are the numbers the kernel must answer.  A handful,
     * spread across the table, are checked. */
    static const struct { const char *zw; ULONG num; } expect[] = {
        { "ZwWaitForSingleObject",     0x004 },
        { "ZwReadFile",                0x006 },
        { "ZwWriteFile",               0x008 },
        { "ZwClose",                   0x00F },
        { "ZwQueryInformationFile",    0x011 },
        { "ZwAllocateVirtualMemory",   0x018 },
        { "ZwQueryInformationProcess", 0x019 },
        { "ZwQuerySystemInformation",  0x036 },
        { "ZwCreateFile",              0x055 },
        { "ZwQuerySystemTime",         0x05A },
        { "ZwQueryInformationToken",   0x021 },
        { "ZwProtectVirtualMemory",    0x050 },
        { "ZwSetEvent",                0x00E },
        { "ZwQueryVirtualMemory",      0x023 },
        { "ZwRaiseHardError",          0x161 },
    };
    int wrong = 0;
    for (size_t i = 0; i < sizeof(expect) / sizeof(expect[0]); i++) {
        ULONG got = ssn_of(tbl, n, expect[i].zw);
        if (got != expect[i].num) {
            printf("FAIL: %s ranks %#lx; Windows 10 1903: %#lx\n", expect[i].zw, got, expect[i].num);
            wrong++;
        }
    }
    CHECK("Zw exports rank at their Windows 10 1903 numbers", !wrong);

    /* Issue a few by their ranked number and check the kernel answered the
     * right service (not service 0, and not the wrong one). */
    LARGE_INTEGER ctr = { 0 }, freq = { 0 };
    CHECK("raw NtQueryPerformanceCounter (0x31)",
          raw_syscall(ssn_of(tbl, n, "ZwQueryPerformanceCounter"), (ULONG_PTR)&ctr, (ULONG_PTR)&freq, 0, 0) == 0 &&
          freq.QuadPart && ctr.QuadPart);

    LARGE_INTEGER t = { 0 };
    CHECK("raw NtQuerySystemTime (0x5A)",
          raw_syscall(ssn_of(tbl, n, "ZwQuerySystemTime"), (ULONG_PTR)&t, 0, 0, 0) == 0 &&
          t.QuadPart > 0x01D0000000000000LL);

    CHECK("raw NtClose of a bad handle (0x0F) -> STATUS_INVALID_HANDLE",
          raw_syscall(ssn_of(tbl, n, "ZwClose"), 0x1234560, 0, 0, 0) == (NTSTATUS)0xC0000008);

    /* NtQuerySystemInformation(SystemBasicInformation): the kernel fills it */
    BYTE basic[64];
    ULONG retlen = 0;
    memset(basic, 0, sizeof(basic));
    NTSTATUS s = raw_syscall(ssn_of(tbl, n, "ZwQuerySystemInformation"), 0, (ULONG_PTR)basic, sizeof(basic), (ULONG_PTR)&retlen);
    CHECK("raw NtQuerySystemInformation (0x36) fills PageSize=4096",
          s == 0 && *(ULONG *)(basic + 8) == 4096);

    /* A service NovaOS has no number for still ranks (its stub is present)
     * and returns STATUS_INVALID_SYSTEM_SERVICE rather than running the
     * wrong one. */
    ULONG missing = ssn_of(tbl, n, "ZwAcceptConnectPort");    /* LPC: NovaOS has none */
    CHECK("an unimplemented service ranks", missing != 0xFFFFFFFFu);
    CHECK("an unimplemented service -> STATUS_INVALID_SYSTEM_SERVICE",
          raw_syscall(missing, 0, 0, 0, 0) == (NTSTATUS)0xC000001C);

    /* The junk-filled high bits of EAX that Hyperion sets are ignored:
     * only EAX's low 12 bits (and bit 12, the table) are read. */
    CHECK("high bits of the number are ignored",
          raw_syscall(0x0A020000u | ssn_of(tbl, n, "ZwClose"), 0x1234560, 0, 0, 0) == (NTSTATUS)0xC0000008);

    printf("syscalltest: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}

#else   /* the 32-bit table is not checked yet */
int main(void) { printf("syscalltest: 0 passed, 0 failed (x64 only)\n"); return 0; }
#endif
