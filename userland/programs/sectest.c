/* sectest.exe — the kernel must refuse hostile system calls, not crash,
 * and enforce tokens and security descriptors when objects are opened */
#include <stdio.h>
#include <string.h>
#include <winternl.h>
#include <windows.h>

NTSYSAPI NTSTATUS NTAPI NtOpenProcessToken(HANDLE p, ACCESS_MASK access, PHANDLE token);
NTSYSAPI NTSTATUS NTAPI NtOpenThreadToken(HANDLE t, ACCESS_MASK access, BOOLEAN self, PHANDLE token);
NTSYSAPI NTSTATUS NTAPI NtDuplicateToken(HANDLE t, ACCESS_MASK access, POBJECT_ATTRIBUTES oa, BOOLEAN effective, TOKEN_TYPE type, PHANDLE out);
NTSYSAPI NTSTATUS NTAPI NtFilterToken(HANDLE t, ULONG flags, PTOKEN_GROUPS disable, PTOKEN_PRIVILEGES del, PTOKEN_GROUPS restrict_sids, PHANDLE out);
NTSYSAPI NTSTATUS NTAPI NtQueryInformationToken(HANDLE t, ULONG cls, PVOID buf, ULONG n, PULONG ret);
NTSYSAPI NTSTATUS NTAPI NtAccessCheck(PSECURITY_DESCRIPTOR sd, HANDLE token, ACCESS_MASK want, PGENERIC_MAPPING map,
                                      PPRIVILEGE_SET privs, PULONG privs_len, PACCESS_MASK granted, NTSTATUS *status);
NTSYSAPI NTSTATUS NTAPI NtQuerySecurityObject(HANDLE h, SECURITY_INFORMATION si, PSECURITY_DESCRIPTOR sd, ULONG len, PULONG need);
NTSYSAPI NTSTATUS NTAPI NtSetSecurityObject(HANDLE h, SECURITY_INFORMATION si, PSECURITY_DESCRIPTOR sd);

static int pass, fail;
#define EXPECT(what, got, want) do { unsigned long g_ = (unsigned long)(got); \
    if (g_ == (unsigned long)(want)) pass++; \
    else { fail++; printf("FAIL: %s -> 0x%08lx (expected 0x%08lx)\n", what, g_, (unsigned long)(want)); } } while (0)

#define STATUS_INVALID_SYSTEM_SERVICE 0xC000001C
#define KERNEL_PTR ((void *)0xFFFFFFFF80100000ULL)
#define EVENT_MODIFY_STATE_ 0x0002
#define STATUS_ACCESS_DENIED 0xC0000022

/* the calling thread impersonates @t (0: stops) */
static NTSTATUS impersonate(HANDLE t)
{
    return NtSetInformationThread(GetCurrentThread(), 5 /* ThreadImpersonationToken */, &t, sizeof(t));
}

/* @t as an impersonation token */
static HANDLE imp_copy(HANDLE t)
{
    SECURITY_QUALITY_OF_SERVICE qos = { sizeof(qos), SecurityImpersonation, 0, FALSE };
    OBJECT_ATTRIBUTES oa;
    memset(&oa, 0, sizeof(oa));
    oa.Length = sizeof(oa);
    oa.SecurityQualityOfService = &qos;
    HANDLE out = 0;
    NTSTATUS s = NtDuplicateToken(t, TOKEN_IMPERSONATE | TOKEN_QUERY, &oa, FALSE, TokenImpersonation, &out);
    return s ? 0 : out;
}

static POBJECT_ATTRIBUTES named(OBJECT_ATTRIBUTES *oa, UNICODE_STRING *us, const WCHAR *name, PSECURITY_DESCRIPTOR sd)
{
    RtlInitUnicodeString(us, name);
    memset(oa, 0, sizeof(*oa));
    oa->Length = sizeof(*oa);
    oa->ObjectName = us;
    oa->SecurityDescriptor = sd;
    return oa;
}

/* Tokens and object security through the native API: a named event only
 * its owner may change (anyone may wait on it) refuses a restricted token
 * and a token whose user is deny-only */
static void tokens(void)
{
    HANDLE pt = 0, ev = 0, h = 0, t = 0;
    BYTE ub[128], buf[512];
    ULONG n = 0;
    EXPECT("NtOpenProcessToken", NtOpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &pt), 0);
    EXPECT("NtQueryInformationToken(TokenUser)", NtQueryInformationToken(pt, TokenUser, ub, sizeof(ub), &n), 0);
    PSID me = ((TOKEN_USER *)ub)->User.Sid;
    EXPECT("NtQueryInformationToken(kernel buffer)", NtQueryInformationToken(pt, TokenUser, KERNEL_PTR, 128, &n),
           STATUS_ACCESS_VIOLATION);
    EXPECT("NtQueryInformationToken(4 bytes)", NtQueryInformationToken(pt, TokenUser, buf, 4, &n), 0xC0000023);
    EXPECT("NtQueryInformationToken(class 999)", NtQueryInformationToken(pt, 999, buf, sizeof(buf), &n), 0xC0000003);
    EXPECT("NtOpenThreadToken (not impersonating)", NtOpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &t),
           0xC000007C /* NO_TOKEN */);
    EXPECT("impersonate a primary token", impersonate(pt), 0xC00000A8 /* BAD_TOKEN_TYPE */);
    NtCreateEvent(&h, EVENT_ALL_ACCESS, 0, NotificationEvent, FALSE);
    EXPECT("impersonate an event", impersonate(h), 0xC0000024 /* OBJECT_TYPE_MISMATCH */);
    NtClose(h);
    EXPECT("ThreadImpersonationToken, 3 bytes", NtSetInformationThread(GetCurrentThread(), 5, &h, 3), 0xC0000004);
    NTSTATUS st = 0;
    ACCESS_MASK granted = 0;
    GENERIC_MAPPING map = { 1, 1, 1, 1 };
    BYTE ps[32];
    ULONG psn = sizeof(ps);
    EXPECT("NtAccessCheck with a primary token", NtAccessCheck(buf, pt, 1, &map, (PPRIVILEGE_SET)ps, &psn, &granted, &st),
           0xC000005C /* NO_IMPERSONATION_TOKEN */);

    /* the protected event: GENERIC_ALL for us, SYNCHRONIZE for everyone */
    SID_IDENTIFIER_AUTHORITY world = { { 0, 0, 0, 0, 0, 1 } };
    PSID everyone;
    AllocateAndInitializeSid(&world, 1, 0, 0, 0, 0, 0, 0, 0, 0, &everyone);
    PACL acl = (PACL)buf;
    InitializeAcl(acl, 256, ACL_REVISION);
    AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, me);
    AddAccessAllowedAce(acl, ACL_REVISION, SYNCHRONIZE, everyone);
    SECURITY_DESCRIPTOR sd;
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, acl, FALSE);
    SetSecurityDescriptorOwner(&sd, me, FALSE);
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING us;
    const WCHAR *name = L"sectest-protected";
    EXPECT("NtCreateEvent(kernel descriptor)", NtCreateEvent(&h, EVENT_ALL_ACCESS, named(&oa, &us, name, KERNEL_PTR),
                                                             NotificationEvent, FALSE), STATUS_ACCESS_VIOLATION);
    SECURITY_DESCRIPTOR bad = sd;
    bad.Revision = 9;
    EXPECT("NtCreateEvent(revision 9 descriptor)", NtCreateEvent(&h, EVENT_ALL_ACCESS, named(&oa, &us, name, &bad),
                                                                 NotificationEvent, FALSE), 0xC0000079);
    EXPECT("NtCreateEvent(protected)", NtCreateEvent(&ev, EVENT_ALL_ACCESS, named(&oa, &us, name, &sd), NotificationEvent, FALSE), 0);

    /* restricted to Everyone: as itself it may change the event, as Everyone only wait */
    struct { DWORD n; SID_AND_ATTRIBUTES g[1]; } rs = { 1, { { everyone, 0 } } };
    HANDLE rt = 0, it;
    EXPECT("NtFilterToken(kernel groups)", NtFilterToken(pt, 0, 0, 0, (PTOKEN_GROUPS)KERNEL_PTR, &rt), STATUS_ACCESS_VIOLATION);
    EXPECT("NtFilterToken(restrict to Everyone)", NtFilterToken(pt, 0, 0, 0, (PTOKEN_GROUPS)&rs, &rt), 0);
    DWORD v = 0;
    EXPECT("TokenHasRestrictions", NtQueryInformationToken(rt, 21, &v, 4, &n) == 0 && v == 1, 1);
    it = imp_copy(rt);
    EXPECT("NtDuplicateToken(impersonation)", it != 0, 1);
    EXPECT("impersonate the restricted token", impersonate(it), 0);
    EXPECT("NtOpenThreadToken (impersonating)", NtOpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &t), 0);
    NtClose(t);
    EXPECT("restricted: NtOpenEvent(MODIFY_STATE) denied", NtOpenEvent(&h, EVENT_MODIFY_STATE_, named(&oa, &us, name, 0)),
           STATUS_ACCESS_DENIED);
    EXPECT("restricted: NtOpenEvent(GENERIC_ALL) denied", NtOpenEvent(&h, GENERIC_ALL, named(&oa, &us, name, 0)),
           STATUS_ACCESS_DENIED);
    EXPECT("restricted: create-or-open denied", NtCreateEvent(&h, EVENT_ALL_ACCESS, named(&oa, &us, name, 0),
                                                              NotificationEvent, FALSE), STATUS_ACCESS_DENIED);
    EXPECT("restricted: NtOpenEvent(SYNCHRONIZE) allowed", NtOpenEvent(&h, SYNCHRONIZE, named(&oa, &us, name, 0)), 0);
    NtClose(h);
    EXPECT("restricted: the owner's WRITE_DAC needs the restricting SIDs too", NtSetSecurityObject(ev, DACL_SECURITY_INFORMATION, &sd),
           STATUS_ACCESS_DENIED);
    EXPECT("stop impersonating", impersonate(0), 0);
    EXPECT("ourselves again: NtOpenEvent(MODIFY_STATE)", NtOpenEvent(&h, EVENT_MODIFY_STATE_, named(&oa, &us, name, 0)), 0);
    NtClose(h);
    NtClose(it);
    NtClose(rt);

    /* our user only for denying: the ACE naming us no longer grants */
    struct { DWORD n; SID_AND_ATTRIBUTES g[1]; } ds = { 1, { { me, 0 } } };
    EXPECT("NtFilterToken(user deny-only)", NtFilterToken(pt, 0, (PTOKEN_GROUPS)&ds, 0, 0, &rt), 0);
    it = imp_copy(rt);
    EXPECT("impersonate the deny-only token", impersonate(it), 0);
    EXPECT("deny-only: NtOpenEvent(MODIFY_STATE) denied", NtOpenEvent(&h, EVENT_MODIFY_STATE_, named(&oa, &us, name, 0)),
           STATUS_ACCESS_DENIED);
    impersonate(0);
    NtClose(it);
    NtClose(rt);

    /* reading the descriptor back */
    ULONG need = 0;
    EXPECT("NtQuerySecurityObject(no buffer)", NtQuerySecurityObject(ev, DACL_SECURITY_INFORMATION, 0, 0, &need), 0xC0000023);
    EXPECT("NtQuerySecurityObject(kernel buffer)", NtQuerySecurityObject(ev, DACL_SECURITY_INFORMATION, KERNEL_PTR, need, &need),
           STATUS_ACCESS_VIOLATION);
    BYTE got[512];
    EXPECT("NtQuerySecurityObject", NtQuerySecurityObject(ev, DACL_SECURITY_INFORMATION, got, sizeof(got), &need), 0);
    BOOL present = FALSE, def;
    PACL dacl = 0;
    GetSecurityDescriptorDacl(got, &present, &dacl, &def);
    EXPECT("the DACL has our two ACEs", present && dacl && dacl->AceCount == 2, 1);
    NtClose(ev);
    NtClose(pt);
    FreeSid(everyone);
}

static long long raw_syscall(long long num, long long a1, long long a2, long long a3, long long a4)
{
    register long long r10 __asm__("r10") = a1;
    register long long r8 __asm__("r8") = a3;
    register long long r9 __asm__("r9") = a4;
    long long ret;
    __asm__ volatile ("syscall" : "=a"(ret), "+r"(r10), "+r"(r8), "+r"(r9)
                      : "a"(num), "d"(a2) : "rcx", "r11", "memory");
    return ret;
}

int main(void)
{
    IO_STATUS_BLOCK io;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);

    /* services the subsystem doesn't provide (legacy/internal ones): the
     * LPC port calls (NtAcceptConnectPort = 0x02), a kernel helper and the
     * numbers past the table */
    EXPECT("NtAcceptConnectPort", raw_syscall(0x02, 0, 0, 0, 0), STATUS_INVALID_SYSTEM_SERVICE);
    EXPECT("kernel helper 0x1F7", raw_syscall(0x1F7, 0, 0, 0, 0), STATUS_INVALID_SYSTEM_SERVICE);
    EXPECT("syscall 0x1FF", raw_syscall(0x1FF, 1, 2, 3, 4), STATUS_INVALID_SYSTEM_SERVICE);
    EXPECT("syscall 0xFFFF", raw_syscall(0xFFFF, 0, 0, 0, 0), STATUS_INVALID_SYSTEM_SERVICE);

    /* kernel addresses as buffers */
    EXPECT("NtWriteFile(kernel buffer)", NtWriteFile(out, 0, 0, 0, &io, KERNEL_PTR, 16, 0, 0), STATUS_ACCESS_VIOLATION);
    EXPECT("NtWriteFile(unmapped buffer)", NtWriteFile(out, 0, 0, 0, &io, (void *)0x1000, 16, 0, 0), STATUS_ACCESS_VIOLATION);
    HANDLE f = CreateFileA("C:\\Welcome.txt", GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0);
    if (f == INVALID_HANDLE_VALUE) f = CreateFileA("C:\\Documents\\Welcome.txt", GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0);
    EXPECT("NtReadFile(kernel buffer)", NtReadFile(f, 0, 0, 0, &io, KERNEL_PTR, 16, 0, 0), STATUS_ACCESS_VIOLATION);
    EXPECT("NtReadFile(kernel IOSB)", NtReadFile(f, 0, 0, 0, (PIO_STATUS_BLOCK)KERNEL_PTR, (char[16]){0}, 16, 0, 0),
           STATUS_ACCESS_VIOLATION);
    CloseHandle(f);
    EXPECT("NtCreateFile(kernel attributes)", NtCreateFile(&f, GENERIC_READ, (POBJECT_ATTRIBUTES)KERNEL_PTR, &io, 0, 0, 0,
                                                           FILE_OPEN, 0, 0, 0), STATUS_ACCESS_VIOLATION);
    PVOID base = KERNEL_PTR;
    SIZE_T size = 4096;
    EXPECT("NtAllocateVirtualMemory(kernel address)", NtAllocateVirtualMemory(NtCurrentProcess(), &base, 0, &size,
                                                                              MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE),
           0xC0000018 /* CONFLICTING_ADDRESSES */);
    base = (PVOID)0x140000000ULL;                                   /* our own image */
    size = 0;
    EXPECT("NtFreeVirtualMemory(image)", NtFreeVirtualMemory(NtCurrentProcess(), &base, &size, MEM_RELEASE),
           0xC00000A0 /* MEMORY_NOT_ALLOCATED */);
    EXPECT("NtClose(bogus handle)", NtClose((HANDLE)0x12345678), 0xC0000008);
    EXPECT("NtClose(kernel pointer handle)", NtClose(KERNEL_PTR), 0xC0000008);

    tokens();

    printf("sectest: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
