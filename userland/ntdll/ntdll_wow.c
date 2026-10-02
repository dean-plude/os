#define NOVA_BUILD_NTDLL
/*
 * ntdll_wow.c — system calls from 32-bit programs (the SysWOW64 ntdll)
 *
 * The kernel's system calls take the 64-bit (x64) forms of their
 * arguments and structures.  A 32-bit program runs in compatibility mode,
 * so this ntdll converts: handles are sign-extended (the pseudo-handles
 * -1 and -2 stay -1 and -2), pointers zero-extended, and every structure
 * that holds a pointer or a pointer-sized value (OBJECT_ATTRIBUTES,
 * UNICODE_STRING, IO_STATUS_BLOCK, CONTEXT, ...) is rebuilt in its 64-bit
 * layout, and the results copied back.  The call itself is int 0x2E with
 * EAX = the system call number and EDX = a block of 64-bit arguments laid
 * out like the 64-bit stack: arguments 1-4 in its first four slots, 5 and
 * up from offset 0x28.  The result comes back in EDX:EAX.
 */
#ifndef _WIN64

#include <winternl.h>
#include <winnt.h>
#include "syscall_numbers.h"

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);

typedef unsigned long long U64;

static U64 sysc(ULONG num, U64 *blk)
{
    ULONG lo, hi;
    __asm__ volatile ("int $0x2e" : "=a"(lo), "=d"(hi) : "a"(num), "d"(blk) : "memory");
    return ((U64)hi << 32) | lo;
}

/* argument i (1-based) goes to slot i-1 for 1..4 and slot i for 5+ */
static U64 callb(ULONG num, const U64 *a, int n)
{
    U64 b[16];
    memset(b, 0, sizeof(b));
    for (int i = 0; i < n; i++) b[i < 4 ? i : i + 1] = a[i];
    return sysc(num, b);
}

#define NARGS_(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, N, ...) N
#define NARGS(...) NARGS_(__VA_ARGS__, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define SC(name, ...)  ((NTSTATUS)callb(SYS_##name, (const U64[]){ __VA_ARGS__ }, NARGS(__VA_ARGS__)))
#define SC0(name)      ((NTSTATUS)callb(SYS_##name, 0, 0))
#define SCP(name, ...) ((LONG_PTR)(LONG)callb(SYS_##name, (const U64[]){ __VA_ARGS__ }, NARGS(__VA_ARGS__)))

#define H(h) ((U64)(long long)(LONG)(ULONG_PTR)(h))     /* handles: sign-extended */
#define P(p) ((U64)(ULONG_PTR)(p))                      /* pointers: zero-extended */
#define U(v) ((U64)(ULONG)(v))                          /* 32-bit unsigned values */
#define S(v) ((U64)(long long)(LONG)(v))                /* 32-bit signed values */

/* -----------------------------------------------------------------------
 * 64-bit structure layouts
 * ----------------------------------------------------------------------- */
typedef struct { USHORT Length, MaximumLength; ULONG pad; U64 Buffer; } US64;
typedef struct { ULONG Length, pad; U64 RootDirectory, ObjectName; ULONG Attributes, pad2; U64 Sd, Sqos; } OA64;
typedef struct { U64 Status, Information; } IOSB64;

typedef struct { OA64 oa; US64 name; } OAC;

static U64 us_in(US64 *d, const UNICODE_STRING *s)
{
    if (!s) return 0;
    d->Length = s->Length; d->MaximumLength = s->MaximumLength; d->pad = 0;
    d->Buffer = P(s->Buffer);
    return P(d);
}

static U64 oa_in(OAC *c, const OBJECT_ATTRIBUTES *oa)
{
    if (!oa) return 0;
    memset(c, 0, sizeof(*c));
    c->oa.Length = sizeof(OA64);
    c->oa.RootDirectory = H(oa->RootDirectory);
    c->oa.ObjectName = us_in(&c->name, oa->ObjectName);
    c->oa.Attributes = oa->Attributes;
    c->oa.Sd = P(oa->SecurityDescriptor);
    c->oa.Sqos = P(oa->SecurityQualityOfService);
    return P(&c->oa);
}

static U64 io_in(IOSB64 *d, const IO_STATUS_BLOCK *io)
{
    if (!io) return 0;
    d->Status = S(io->Status);
    d->Information = P(io->Information);
    return P(d);
}

static void io_out(IO_STATUS_BLOCK *io, const IOSB64 *d)
{
    if (!io) return;
    io->Status = (NTSTATUS)d->Status;
    io->Information = (ULONG_PTR)d->Information;
}

/* pointer-sized in/out values (HANDLE*, PVOID*, SIZE_T*) */
typedef struct { U64 v; void *p; int handle; } Box;
static U64 box_in(Box *b, void *p, int handle)
{
    b->p = p;
    b->handle = handle;
    if (!p) return 0;
    ULONG_PTR v = *(ULONG_PTR *)p;
    b->v = handle ? H(v) : P(v);
    return P(&b->v);
}
static void box_out(const Box *b)
{
    if (b->p) *(ULONG_PTR *)b->p = (ULONG_PTR)b->v;
}
#define HBOX(b, p) box_in(&(b), (void *)(p), 1)
#define PBOX(b, p) box_in(&(b), (void *)(p), 0)

/* -----------------------------------------------------------------------
 * CONTEXT and EXCEPTION_RECORD (also used by the exception dispatcher)
 * ----------------------------------------------------------------------- */
#define C64_SIZE    0x4D0
#define C64_FLAGS   0x30
#define C64_MXCSR   0x34
#define C64_SEGCS   0x38
#define C64_EFLAGS  0x44
#define C64_DR0     0x48
#define C64_RAX     0x78
#define C64_RIP     0xF8
#define C64_FLT     0x100
#define R64_SIZE    0x98

static U64 g64(const BYTE *b, int off) { U64 v; memcpy(&v, b + off, 8); return v; }
static void p64(BYTE *b, int off, U64 v) { memcpy(b + off, &v, 8); }

/* x64 register order: rax rcx rdx rbx rsp rbp rsi rdi */
void nova_context_from64(CONTEXT *c, const BYTE *x)
{
    ULONG f;
    memcpy(&f, x + C64_FLAGS, 4);
    memset(c, 0, sizeof(*c));
    c->ContextFlags = CONTEXT_i386 | (f & 0x17);
    c->Eax = (DWORD)g64(x, C64_RAX + 0x00);
    c->Ecx = (DWORD)g64(x, C64_RAX + 0x08);
    c->Edx = (DWORD)g64(x, C64_RAX + 0x10);
    c->Ebx = (DWORD)g64(x, C64_RAX + 0x18);
    c->Esp = (DWORD)g64(x, C64_RAX + 0x20);
    c->Ebp = (DWORD)g64(x, C64_RAX + 0x28);
    c->Esi = (DWORD)g64(x, C64_RAX + 0x30);
    c->Edi = (DWORD)g64(x, C64_RAX + 0x38);
    c->Eip = (DWORD)g64(x, C64_RIP);
    memcpy(&c->EFlags, x + C64_EFLAGS, 4);
    USHORT seg[6];
    memcpy(seg, x + C64_SEGCS, 12);                  /* cs ds es fs gs ss */
    c->SegCs = seg[0]; c->SegDs = seg[1]; c->SegEs = seg[2];
    c->SegFs = seg[3]; c->SegGs = seg[4]; c->SegSs = seg[5];
    c->Dr0 = (DWORD)g64(x, C64_DR0);      c->Dr1 = (DWORD)g64(x, C64_DR0 + 8);
    c->Dr2 = (DWORD)g64(x, C64_DR0 + 16); c->Dr3 = (DWORD)g64(x, C64_DR0 + 24);
    c->Dr6 = (DWORD)g64(x, C64_DR0 + 32); c->Dr7 = (DWORD)g64(x, C64_DR0 + 40);
    if (f & 0x8) {                                   /* the FXSAVE image */
        const BYTE *fx = x + C64_FLT;
        c->ContextFlags |= CONTEXT_FLOATING_POINT | CONTEXT_EXTENDED_REGISTERS;
        memcpy(c->ExtendedRegisters, fx, 512);
        USHORT fcw, fsw;
        memcpy(&fcw, fx, 2); memcpy(&fsw, fx + 2, 2);
        c->FloatSave.ControlWord = 0xFFFF0000u | fcw;
        c->FloatSave.StatusWord = 0xFFFF0000u | fsw;
        DWORD tag = 0;                               /* abridged tag: 1 = valid → 00, 0 = empty → 11 */
        for (int i = 0; i < 8; i++) if (!(fx[4] & (1 << i))) tag |= 3u << (2 * i);
        c->FloatSave.TagWord = 0xFFFF0000u | tag;
        memcpy(&c->FloatSave.ErrorOffset, fx + 8, 4);
        memcpy(&c->FloatSave.DataOffset, fx + 16, 4);
        for (int i = 0; i < 8; i++) memcpy(c->FloatSave.RegisterArea + 10 * i, fx + 32 + 16 * i, 10);
    }
}

void nova_context_to64(BYTE *x, const CONTEXT *c)
{
    memset(x, 0, C64_SIZE);
    ULONG f = 0x100000 | (c->ContextFlags & 0x17);
    if ((c->ContextFlags & CONTEXT_EXTENDED_REGISTERS) == CONTEXT_EXTENDED_REGISTERS) {
        f |= 0x8;
        memcpy(x + C64_FLT, c->ExtendedRegisters, 512);
        memcpy(x + C64_MXCSR, c->ExtendedRegisters + 24, 4);
    }
    memcpy(x + C64_FLAGS, &f, 4);
    p64(x, C64_RAX + 0x00, c->Eax); p64(x, C64_RAX + 0x08, c->Ecx);
    p64(x, C64_RAX + 0x10, c->Edx); p64(x, C64_RAX + 0x18, c->Ebx);
    p64(x, C64_RAX + 0x20, c->Esp); p64(x, C64_RAX + 0x28, c->Ebp);
    p64(x, C64_RAX + 0x30, c->Esi); p64(x, C64_RAX + 0x38, c->Edi);
    p64(x, C64_RIP, c->Eip);
    memcpy(x + C64_EFLAGS, &c->EFlags, 4);
    USHORT seg[6] = { (USHORT)c->SegCs, (USHORT)c->SegDs, (USHORT)c->SegEs,
                      (USHORT)c->SegFs, (USHORT)c->SegGs, (USHORT)c->SegSs };
    memcpy(x + C64_SEGCS, seg, 12);
    p64(x, C64_DR0, c->Dr0); p64(x, C64_DR0 + 8, c->Dr1); p64(x, C64_DR0 + 16, c->Dr2);
    p64(x, C64_DR0 + 24, c->Dr3); p64(x, C64_DR0 + 32, c->Dr6); p64(x, C64_DR0 + 40, c->Dr7);
}

void nova_record_from64(EXCEPTION_RECORD *r, const BYTE *x)
{
    memset(r, 0, sizeof(*r));
    memcpy(&r->ExceptionCode, x, 4);
    memcpy(&r->ExceptionFlags, x + 4, 4);
    r->ExceptionAddress = (PVOID)(ULONG_PTR)g64(x, 16);
    ULONG n;
    memcpy(&n, x + 24, 4);
    if (n > EXCEPTION_MAXIMUM_PARAMETERS) n = EXCEPTION_MAXIMUM_PARAMETERS;
    r->NumberParameters = n;
    for (ULONG i = 0; i < n; i++) r->ExceptionInformation[i] = (ULONG_PTR)g64(x, 32 + 8 * i);
}

static void record_to64(BYTE *x, const EXCEPTION_RECORD *r)
{
    memset(x, 0, R64_SIZE);
    memcpy(x, &r->ExceptionCode, 4);
    memcpy(x + 4, &r->ExceptionFlags, 4);
    p64(x, 16, P(r->ExceptionAddress));
    ULONG n = r->NumberParameters > EXCEPTION_MAXIMUM_PARAMETERS ? EXCEPTION_MAXIMUM_PARAMETERS : r->NumberParameters;
    memcpy(x + 24, &n, 4);
    for (ULONG i = 0; i < n; i++) p64(x, 32 + 8 * i, P(r->ExceptionInformation[i]));
}

/* -----------------------------------------------------------------------
 * Files
 * ----------------------------------------------------------------------- */
NTSTATUS NTAPI NtClose(HANDLE h) { return SC(NtClose, H(h)); }

NTSTATUS NTAPI NtCreateFile(PHANDLE h, ULONG access, POBJECT_ATTRIBUTES oa, PIO_STATUS_BLOCK io,
                            PLARGE_INTEGER alloc, ULONG attrs, ULONG share, ULONG disposition,
                            ULONG options, PVOID ea, ULONG ealen)
{
    Box hb; OAC oc; IOSB64 iob;
    NTSTATUS s = SC(NtCreateFile, HBOX(hb, h), U(access), oa_in(&oc, oa), io_in(&iob, io), P(alloc), U(attrs),
                    U(share), U(disposition), U(options), P(ea), U(ealen));
    box_out(&hb); io_out(io, &iob);
    return s;
}

NTSTATUS NTAPI NtOpenFile(PHANDLE h, ULONG access, POBJECT_ATTRIBUTES oa, PIO_STATUS_BLOCK io,
                          ULONG share, ULONG options)
{
    Box hb; OAC oc; IOSB64 iob;
    NTSTATUS s = SC(NtOpenFile, HBOX(hb, h), U(access), oa_in(&oc, oa), io_in(&iob, io), U(share), U(options));
    box_out(&hb); io_out(io, &iob);
    return s;
}

/* Reads, writes and file-system controls may finish after the call
 * returns (overlapped I/O), so they get the program's own status block:
 * bit 63 tells the kernel it is the 32-bit layout */
#define IO32(io) ((io) ? P(io) | (1ULL << 63) : 0)

NTSTATUS NTAPI NtReadFile(HANDLE h, HANDLE ev, PVOID apc, PVOID ctx, PIO_STATUS_BLOCK io,
                          PVOID buf, ULONG len, PLARGE_INTEGER off, PULONG key)
{
    return SC(NtReadFile, H(h), H(ev), P(apc), P(ctx), IO32(io), P(buf), U(len), P(off), P(key));
}

NTSTATUS NTAPI NtWriteFile(HANDLE h, HANDLE ev, PVOID apc, PVOID ctx, PIO_STATUS_BLOCK io,
                           const VOID *buf, ULONG len, PLARGE_INTEGER off, PULONG key)
{
    return SC(NtWriteFile, H(h), H(ev), P(apc), P(ctx), IO32(io), P(buf), U(len), P(off), P(key));
}

NTSTATUS NTAPI NtFsControlFile(HANDLE h, HANDLE ev, PVOID apc, PVOID ctx, PIO_STATUS_BLOCK io, ULONG code,
                               PVOID in, ULONG in_len, PVOID out, ULONG out_len)
{
    return SC(NtFsControlFile, H(h), H(ev), P(apc), P(ctx), IO32(io), U(code), P(in), U(in_len), P(out), U(out_len));
}

NTSTATUS NTAPI NtCreateNamedPipeFile(PHANDLE h, ULONG access, POBJECT_ATTRIBUTES oa, PIO_STATUS_BLOCK io,
                                     ULONG share, ULONG disposition, ULONG options, ULONG type, ULONG read_mode,
                                     ULONG completion, ULONG max_inst, ULONG in_quota, ULONG out_quota,
                                     PLARGE_INTEGER timeout)
{
    Box hb; OAC oc; IOSB64 iob;
    NTSTATUS s = SC(NtCreateNamedPipeFile, HBOX(hb, h), U(access), oa_in(&oc, oa), io_in(&iob, io), U(share),
                    U(disposition), U(options), U(type), U(read_mode), U(completion), U(max_inst), U(in_quota),
                    U(out_quota), P(timeout));
    box_out(&hb); io_out(io, &iob);
    return s;
}

NTSTATUS NTAPI NtCancelIoFile(HANDLE h, PIO_STATUS_BLOCK io)
{
    IOSB64 iob;
    NTSTATUS s = SC(NtCancelIoFile, H(h), io_in(&iob, io));
    io_out(io, &iob);
    return s;
}

NTSTATUS NTAPI NtCancelIoFileEx(HANDLE h, PIO_STATUS_BLOCK req, PIO_STATUS_BLOCK io)
{
    IOSB64 iob;
    NTSTATUS s = SC(NtCancelIoFileEx, H(h), IO32(req), io_in(&iob, io));
    io_out(io, &iob);
    return s;
}

NTSTATUS NTAPI NtSetInformationObject(HANDLE h, ULONG cls, PVOID info, ULONG len)
{
    return SC(NtSetInformationObject, H(h), U(cls), P(info), U(len));
}

NTSTATUS NTAPI NtOpenProcess(PHANDLE h, ULONG access, POBJECT_ATTRIBUTES oa, CLIENT_ID *cid)
{
    Box hb; OAC oc;
    U64 c[2] = { cid ? (U64)(ULONG_PTR)cid->UniqueProcess : 0, cid ? (U64)(ULONG_PTR)cid->UniqueThread : 0 };
    NTSTATUS s = SC(NtOpenProcess, HBOX(hb, h), U(access), oa_in(&oc, oa), cid ? P(c) : 0);
    box_out(&hb);
    return s;
}

NTSTATUS NTAPI NtOpenThread(PHANDLE h, ULONG access, POBJECT_ATTRIBUTES oa, CLIENT_ID *cid)
{
    Box hb; OAC oc;
    U64 c[2] = { cid ? (U64)(ULONG_PTR)cid->UniqueProcess : 0, cid ? (U64)(ULONG_PTR)cid->UniqueThread : 0 };
    NTSTATUS s = SC(NtOpenThread, HBOX(hb, h), U(access), oa_in(&oc, oa), cid ? P(c) : 0);
    box_out(&hb);
    return s;
}

static NTSTATUS copy_vm(ULONG num, HANDLE p, PVOID base, PVOID buf, SIZE_T n, PSIZE_T done)
{
    U64 got = 0;
    U64 a[5] = { H(p), P(base), P(buf), U(n), P(&got) };
    NTSTATUS s = (NTSTATUS)callb(num, a, 5);
    if (done) *done = (SIZE_T)got;
    return s;
}
NTSTATUS NTAPI NtReadVirtualMemory(HANDLE p, PVOID base, PVOID buf, SIZE_T n, PSIZE_T done)  { return copy_vm(SYS_NtReadVirtualMemory, p, base, buf, n, done); }
NTSTATUS NTAPI NtWriteVirtualMemory(HANDLE p, PVOID base, PVOID buf, SIZE_T n, PSIZE_T done) { return copy_vm(SYS_NtWriteVirtualMemory, p, base, buf, n, done); }

LONG_PTR NTAPI NtNovaClipboard(ULONG op, ULONG_PTR a, PVOID b, ULONG_PTR c, const char *name)
{
    return SCP(NtNovaClipboard, U(op), op == 0 ? H(a) : U(a), P(b), U(c), P(name));
}

NTSTATUS NTAPI NtQueryObject(HANDLE h, ULONG cls, PVOID info, ULONG len, PULONG ret)
{
    return SC(NtQueryObject, H(h), U(cls), P(info), U(len), P(ret));
}

NTSTATUS NTAPI NtQueryInformationFile(HANDLE h, PIO_STATUS_BLOCK io, PVOID info, ULONG len, ULONG cls)
{
    IOSB64 iob;
    NTSTATUS s = SC(NtQueryInformationFile, H(h), io_in(&iob, io), P(info), U(len), U(cls));
    io_out(io, &iob);
    return s;
}

NTSTATUS NTAPI NtSetInformationFile(HANDLE h, PIO_STATUS_BLOCK io, PVOID info, ULONG len, ULONG cls)
{
    IOSB64 iob;
    NTSTATUS s;
    if ((cls == 10 || cls == 11) && info && len >= 12) {
        /* FILE_RENAME/LINK_INFORMATION: { BOOLEAN Replace; HANDLE Root; ULONG NameLength; WCHAR Name[] } */
        const BYTE *in = info;
        ULONG nlen;
        memcpy(&nlen, in + 8, 4);
        if (nlen > len - 12) nlen = len - 12;
        BYTE *x = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, 24 + nlen + 2);
        if (!x) return STATUS_NO_MEMORY;
        x[0] = in[0];
        HANDLE root;
        memcpy(&root, in + 4, 4);
        p64(x, 8, H(root));
        memcpy(x + 16, &nlen, 4);
        memcpy(x + 20, in + 12, nlen);
        s = SC(NtSetInformationFile, H(h), io_in(&iob, io), P(x), U(20 + nlen), U(cls));
        RtlFreeHeap(RtlGetProcessHeap(), 0, x);
    } else {
        s = SC(NtSetInformationFile, H(h), io_in(&iob, io), P(info), U(len), U(cls));
    }
    io_out(io, &iob);
    return s;
}

NTSTATUS NTAPI NtQueryAttributesFile(POBJECT_ATTRIBUTES oa, FILE_BASIC_INFORMATION *info)
{
    OAC oc;
    return SC(NtQueryAttributesFile, oa_in(&oc, oa), P(info));
}

NTSTATUS NTAPI NtQueryFullAttributesFile(POBJECT_ATTRIBUTES oa, PVOID info)
{
    OAC oc;
    return SC(NtQueryFullAttributesFile, oa_in(&oc, oa), P(info));   /* the same layout in both */
}

/* Tokens */
NTSTATUS NTAPI NtOpenProcessToken(HANDLE p, ACCESS_MASK access, PHANDLE token)
{
    Box hb;
    NTSTATUS s = SC(NtOpenProcessToken, H(p), U(access), HBOX(hb, token));
    box_out(&hb);
    return s;
}
NTSTATUS NTAPI NtOpenProcessTokenEx(HANDLE p, ACCESS_MASK access, ULONG attrs, PHANDLE token)
{
    Box hb;
    NTSTATUS s = SC(NtOpenProcessTokenEx, H(p), U(access), U(attrs), HBOX(hb, token));
    box_out(&hb);
    return s;
}
NTSTATUS NTAPI NtOpenThreadToken(HANDLE t, ACCESS_MASK access, BOOLEAN self, PHANDLE token)
{
    Box hb;
    NTSTATUS s = SC(NtOpenThreadToken, H(t), U(access), U(self), HBOX(hb, token));
    box_out(&hb);
    return s;
}
NTSTATUS NTAPI NtOpenThreadTokenEx(HANDLE t, ACCESS_MASK access, BOOLEAN self, ULONG attrs, PHANDLE token)
{
    Box hb;
    NTSTATUS s = SC(NtOpenThreadTokenEx, H(t), U(access), U(self), U(attrs), HBOX(hb, token));
    box_out(&hb);
    return s;
}
NTSTATUS NTAPI NtImpersonateAnonymousToken(HANDLE t) { return SC(NtImpersonateAnonymousToken, H(t)); }

NTSTATUS NTAPI NtQueryDirectoryFile(HANDLE h, HANDLE ev, PVOID apc, PVOID ctx, PIO_STATUS_BLOCK io,
                                    PVOID info, ULONG len, ULONG cls, BOOLEAN single,
                                    PUNICODE_STRING name, BOOLEAN restart)
{
    IOSB64 iob; US64 nm;
    NTSTATUS s = SC(NtQueryDirectoryFile, H(h), H(ev), P(apc), P(ctx), io_in(&iob, io), P(info), U(len), U(cls),
                    U(single), us_in(&nm, name), U(restart));
    io_out(io, &iob);
    return s;
}

NTSTATUS NTAPI NtQueryVolumeInformationFile(HANDLE h, PIO_STATUS_BLOCK io, PVOID info, ULONG len, ULONG cls)
{
    IOSB64 iob;
    NTSTATUS s = SC(NtQueryVolumeInformationFile, H(h), io_in(&iob, io), P(info), U(len), U(cls));
    io_out(io, &iob);
    return s;
}

NTSTATUS NTAPI NtNovaWatchDirectory(HANDLE dir, BOOLEAN subtree, HANDLE event, ULONG remove)
{
    return SC(NtNovaWatchDirectory, H(dir), U(subtree), H(event), U(remove));
}

/* -----------------------------------------------------------------------
 * Memory
 * ----------------------------------------------------------------------- */
NTSTATUS NTAPI NtAllocateVirtualMemory(HANDLE p, PVOID *base, ULONG_PTR zero, PSIZE_T size, ULONG type, ULONG prot)
{
    Box b, sz;
    NTSTATUS s = SC(NtAllocateVirtualMemory, H(p), PBOX(b, base), P(zero), PBOX(sz, size), U(type), U(prot));
    box_out(&b); box_out(&sz);
    return s;
}

NTSTATUS NTAPI NtFreeVirtualMemory(HANDLE p, PVOID *base, PSIZE_T size, ULONG type)
{
    Box b, sz;
    NTSTATUS s = SC(NtFreeVirtualMemory, H(p), PBOX(b, base), PBOX(sz, size), U(type));
    box_out(&b); box_out(&sz);
    return s;
}

NTSTATUS NTAPI NtProtectVirtualMemory(HANDLE p, PVOID *base, PSIZE_T size, ULONG prot, PULONG old)
{
    Box b, sz;
    NTSTATUS s = SC(NtProtectVirtualMemory, H(p), PBOX(b, base), PBOX(sz, size), U(prot), P(old));
    box_out(&b); box_out(&sz);
    return s;
}

NTSTATUS NTAPI NtQueryVirtualMemory(HANDLE p, PVOID addr, int cls, PVOID buf, SIZE_T n, PSIZE_T ret)
{
    if (cls != 0) return SC(NtQueryVirtualMemory, H(p), P(addr), S(cls), P(buf), U(n), 0);
    /* MEMORY_BASIC_INFORMATION: 48 bytes in x64, 28 here */
    if (n < 28) return 0xC0000004;                   /* STATUS_INFO_LENGTH_MISMATCH */
    BYTE m[48];
    U64 got = 0;
    NTSTATUS s = SC(NtQueryVirtualMemory, H(p), P(addr), 0, P(m), 48, P(&got));
    if (NT_SUCCESS(s)) {
        ULONG *o = buf;
        o[0] = (ULONG)g64(m, 0);                     /* BaseAddress */
        o[1] = (ULONG)g64(m, 8);                     /* AllocationBase */
        memcpy(&o[2], m + 16, 4);                    /* AllocationProtect */
        o[3] = (ULONG)g64(m, 24);                    /* RegionSize */
        memcpy(&o[4], m + 32, 12);                   /* State, Protect, Type */
        if (ret) *ret = 28;
    }
    return s;
}

NTSTATUS NTAPI NtNovaFlushView(PVOID base) { return SC(NtNovaFlushView, P(base)); }
/* (INPUT_RECORD has the same layout in both) */
NTSTATUS NTAPI NtNovaConsole(HANDLE h, ULONG op, PVOID buf, ULONG len, PULONG res) { return SC(NtNovaConsole, H(h), U(op), P(buf), U(len), P(res)); }

/* The extended forms, without their address requirements (32-bit programs
 * have one small address space anyway) */
NTSTATUS NTAPI NtAllocateVirtualMemoryEx(HANDLE p, PVOID *base, PSIZE_T size, ULONG type, ULONG prot, PVOID params, ULONG n)
{
    (void)params; (void)n;
    return NtAllocateVirtualMemory(p, base, 0, size, type, prot);
}
NTSTATUS NTAPI NtMapViewOfSectionEx(HANDLE sec, HANDLE p, PVOID *base, PLARGE_INTEGER off, PSIZE_T size, ULONG type,
                                    ULONG prot, PVOID params, ULONG n)
{
    (void)params; (void)n;
    return NtMapViewOfSection(sec, p, base, 0, 0, off, size, 2 /* ViewUnmap */, type, prot);
}

/* -----------------------------------------------------------------------
 * Sections
 * ----------------------------------------------------------------------- */
NTSTATUS NTAPI NtCreateSection(PHANDLE h, ULONG access, POBJECT_ATTRIBUTES oa, PLARGE_INTEGER size, ULONG protect,
                               ULONG attrs, HANDLE file)
{
    Box hb; OAC oc;
    NTSTATUS s = SC(NtCreateSection, HBOX(hb, h), U(access), oa_in(&oc, oa), P(size), U(protect), U(attrs), H(file));
    box_out(&hb);
    return s;
}

NTSTATUS NTAPI NtOpenSection(PHANDLE h, ULONG access, POBJECT_ATTRIBUTES oa)
{
    Box hb; OAC oc;
    NTSTATUS s = SC(NtOpenSection, HBOX(hb, h), U(access), oa_in(&oc, oa));
    box_out(&hb);
    return s;
}

NTSTATUS NTAPI NtMapViewOfSection(HANDLE sec, HANDLE proc, PVOID *base, ULONG_PTR zero_bits, SIZE_T commit,
                                  PLARGE_INTEGER offset, PSIZE_T view, ULONG inherit, ULONG type, ULONG protect)
{
    Box b, v;
    NTSTATUS s = SC(NtMapViewOfSection, H(sec), H(proc), PBOX(b, base), P(zero_bits), P(commit), P(offset),
                    PBOX(v, view), U(inherit), U(type), U(protect));
    box_out(&b); box_out(&v);
    return s;
}

NTSTATUS NTAPI NtUnmapViewOfSection(HANDLE proc, PVOID base) { return SC(NtUnmapViewOfSection, H(proc), P(base)); }

/* -----------------------------------------------------------------------
 * Processes and threads
 * ----------------------------------------------------------------------- */
NTSTATUS NTAPI NtNovaCreateProcess(const char *image, const char *cmdline, const char *dir, NOVA_CREATE_PROCESS *io)
{
    U64 x[12];         /* { StdHandle[3], Process, Thread, Pid, Tid, Flags, Environment, EnvironmentSize, RuntimeData, RuntimeDataSize } */
    for (int i = 0; i < 3; i++) x[i] = H(io->StdHandle[i]);
    x[3] = H(io->Process); x[4] = H(io->Thread);
    x[5] = io->ProcessId; x[6] = io->ThreadId;
    x[7] = io->Flags; x[8] = P(io->Environment); x[9] = io->EnvironmentSize;
    x[10] = P(io->RuntimeData); x[11] = io->RuntimeDataSize;
    NTSTATUS s = SC(NtNovaCreateProcess, P(image), P(cmdline), P(dir), P(x));
    for (int i = 0; i < 3; i++) io->StdHandle[i] = (HANDLE)(ULONG_PTR)x[i];
    io->Process = (HANDLE)(ULONG_PTR)x[3]; io->Thread = (HANDLE)(ULONG_PTR)x[4];
    io->ProcessId = x[5]; io->ThreadId = x[6];
    return s;
}

NTSTATUS NTAPI NtNovaProcessInfo(HANDLE p, ULONG64 out[3]) { return SC(NtNovaProcessInfo, H(p), P(out)); }
NTSTATUS NTAPI NtNovaProcessList(NOVA_PROCESS_ENTRY *buf, ULONG max, PULONG count)
{
    return SC(NtNovaProcessList, P(buf), U(max), P(count));
}
NTSTATUS NTAPI NtTerminateProcess(HANDLE p, NTSTATUS status) { return SC(NtTerminateProcess, H(p), U(status)); }

NTSTATUS NTAPI NtQuerySection(HANDLE h, ULONG cls, PVOID info, SIZE_T len, PSIZE_T ret)
{
    if (cls != 0) return SC(NtQuerySection, H(h), U(cls), P(info), U(len), P(ret));
    if (len < 16) return 0xC0000004;
    U64 b[3];
    NTSTATUS s = SC(NtQuerySection, H(h), 0, P(b), 24, 0);
    if (NT_SUCCESS(s)) {
        ULONG *o = info;
        o[0] = (ULONG)b[0];
        o[1] = (ULONG)b[1];
        *(U64 *)(o + 2) = b[2];
        if (ret) *ret = 16;
    }
    return s;
}

NTSTATUS NTAPI NtQueryInformationProcess(HANDLE h, ULONG cls, PVOID info, ULONG len, PULONG ret)
{
    if (cls == 26) {                                 /* ProcessWow64Information: yes, a 32-bit process */
        if (len < 4) return 0xC0000004;
        *(ULONG_PTR *)info = (ULONG_PTR)RtlGetCurrentPeb();
        if (ret) *ret = 4;
        return STATUS_SUCCESS;
    }
    if (cls != 0) return SC(NtQueryInformationProcess, H(h), U(cls), P(info), U(len), P(ret));
    if (len < sizeof(PROCESS_BASIC_INFORMATION)) return 0xC0000004;
    U64 b[6];
    ULONG got;
    NTSTATUS s = SC(NtQueryInformationProcess, H(h), 0, P(b), 48, P(&got));
    if (NT_SUCCESS(s)) {
        PROCESS_BASIC_INFORMATION *o = info;
        o->ExitStatus = (NTSTATUS)b[0];
        o->PebBaseAddress = (PPEB)(ULONG_PTR)b[1];
        o->AffinityMask = (ULONG_PTR)b[2];
        o->BasePriority = (LONG)b[3];
        o->UniqueProcessId = (ULONG_PTR)b[4];
        o->InheritedFromUniqueProcessId = (ULONG_PTR)b[5];
        if (ret) *ret = sizeof(*o);
    }
    return s;
}

NTSTATUS NTAPI NtCreateThreadEx(PHANDLE h, ULONG access, POBJECT_ATTRIBUTES oa, HANDLE process,
                                PVOID start, PVOID arg, ULONG flags, SIZE_T zero_bits,
                                SIZE_T stack_size, SIZE_T max_stack_size, PVOID attrs)
{
    Box hb; OAC oc;
    (void)attrs;
    NTSTATUS s = SC(NtCreateThreadEx, HBOX(hb, h), U(access), oa_in(&oc, oa), H(process), P(start), P(arg),
                    U(flags), P(zero_bits), P(stack_size), P(max_stack_size), 0);
    box_out(&hb);
    return s;
}

NTSTATUS NTAPI NtTerminateThread(HANDLE h, NTSTATUS status) { return SC(NtTerminateThread, H(h), U(status)); }
NTSTATUS NTAPI NtResumeThread(HANDLE h, PULONG prev)  { return SC(NtResumeThread, H(h), P(prev)); }
NTSTATUS NTAPI NtSuspendThread(HANDLE h, PULONG prev) { return SC(NtSuspendThread, H(h), P(prev)); }

NTSTATUS NTAPI NtQueryInformationThread(HANDLE h, ULONG cls, PVOID info, ULONG len, PULONG ret)
{
    if (cls != 0) return SC(NtQueryInformationThread, H(h), U(cls), P(info), U(len), P(ret));
    if (len < sizeof(THREAD_BASIC_INFORMATION)) return 0xC0000004;
    U64 b[6];
    ULONG got;
    NTSTATUS s = SC(NtQueryInformationThread, H(h), 0, P(b), 48, P(&got));
    if (NT_SUCCESS(s)) {
        THREAD_BASIC_INFORMATION *o = info;
        o->ExitStatus = (NTSTATUS)b[0];
        o->TebBaseAddress = (PVOID)(ULONG_PTR)b[1];
        o->ClientId.UniqueProcess = (HANDLE)(ULONG_PTR)b[2];
        o->ClientId.UniqueThread = (HANDLE)(ULONG_PTR)b[3];
        o->AffinityMask = (ULONG_PTR)b[4];
        o->Priority = (LONG)(ULONG)b[5];
        o->BasePriority = (LONG)(b[5] >> 32);
        if (ret) *ret = sizeof(*o);
    }
    return s;
}

NTSTATUS NTAPI NtSetInformationProcess(HANDLE h, ULONG cls, PVOID info, ULONG len)
{
    return SC(NtSetInformationProcess, H(h), U(cls), P(info), U(len));
}

NTSTATUS NTAPI NtSetInformationThread(HANDLE h, ULONG cls, PVOID info, ULONG len)
{
    return SC(NtSetInformationThread, H(h), U(cls), P(info), U(len));
}

NTSTATUS NTAPI NtGetContextThread(HANDLE t, PCONTEXT c)
{
    BYTE x[C64_SIZE] __attribute__((aligned(16)));
    memset(x, 0, sizeof(x));
    NTSTATUS s = SC(NtGetContextThread, H(t), P(x));
    if (NT_SUCCESS(s)) {
        DWORD want = c->ContextFlags;
        CONTEXT tmp;
        nova_context_from64(&tmp, x);
        tmp.ContextFlags &= want | CONTEXT_i386;
        *c = tmp;
    }
    return s;
}

NTSTATUS NTAPI NtSetContextThread(HANDLE t, const CONTEXT *c)
{
    BYTE x[C64_SIZE] __attribute__((aligned(16)));
    nova_context_to64(x, c);
    return SC(NtSetContextThread, H(t), P(x));
}

NTSTATUS NTAPI NtContinue(PCONTEXT ctx, BOOLEAN alert)
{
    BYTE x[C64_SIZE] __attribute__((aligned(16)));
    nova_context_to64(x, ctx);
    return SC(NtContinue, P(x), U(alert));
}

NTSTATUS NTAPI NtRaiseException(PEXCEPTION_RECORD rec, PCONTEXT ctx, BOOLEAN first_chance)
{
    BYTE x[C64_SIZE] __attribute__((aligned(16))), r[R64_SIZE];
    nova_context_to64(x, ctx);
    record_to64(r, rec);
    return SC(NtRaiseException, P(r), P(x), U(first_chance));
}

/* -----------------------------------------------------------------------
 * Synchronization objects
 * ----------------------------------------------------------------------- */
#define OPEN3(name)                                                             \
    NTSTATUS NTAPI name(PHANDLE h, ULONG access, POBJECT_ATTRIBUTES oa)         \
    {                                                                           \
        Box hb; OAC oc;                                                         \
        NTSTATUS s = SC(name, HBOX(hb, h), U(access), oa_in(&oc, oa));          \
        box_out(&hb);                                                           \
        return s;                                                               \
    }
OPEN3(NtOpenEvent)
OPEN3(NtOpenMutant)
OPEN3(NtOpenSemaphore)
OPEN3(NtOpenKey)

NTSTATUS NTAPI NtCreateEvent(PHANDLE h, ULONG access, POBJECT_ATTRIBUTES oa, EVENT_TYPE type, BOOLEAN state)
{
    Box hb; OAC oc;
    NTSTATUS s = SC(NtCreateEvent, HBOX(hb, h), U(access), oa_in(&oc, oa), U(type), U(state));
    box_out(&hb);
    return s;
}

NTSTATUS NTAPI NtCreateMutant(PHANDLE h, ULONG access, POBJECT_ATTRIBUTES oa, BOOLEAN owner)
{
    Box hb; OAC oc;
    NTSTATUS s = SC(NtCreateMutant, HBOX(hb, h), U(access), oa_in(&oc, oa), U(owner));
    box_out(&hb);
    return s;
}

NTSTATUS NTAPI NtCreateSemaphore(PHANDLE h, ULONG access, POBJECT_ATTRIBUTES oa, LONG init, LONG max)
{
    Box hb; OAC oc;
    NTSTATUS s = SC(NtCreateSemaphore, HBOX(hb, h), U(access), oa_in(&oc, oa), S(init), S(max));
    box_out(&hb);
    return s;
}

NTSTATUS NTAPI NtSetEvent(HANDLE h, PLONG prev)      { return SC(NtSetEvent, H(h), P(prev)); }
NTSTATUS NTAPI NtResetEvent(HANDLE h, PLONG prev)    { return SC(NtResetEvent, H(h), P(prev)); }
NTSTATUS NTAPI NtClearEvent(HANDLE h)                { return SC(NtClearEvent, H(h)); }
NTSTATUS NTAPI NtReleaseMutant(HANDLE h, PLONG prev) { return SC(NtReleaseMutant, H(h), P(prev)); }
NTSTATUS NTAPI NtReleaseSemaphore(HANDLE h, LONG count, PLONG prev)
{
    return SC(NtReleaseSemaphore, H(h), S(count), P(prev));
}

NTSTATUS NTAPI NtWaitForSingleObject(HANDLE h, BOOLEAN alertable, PLARGE_INTEGER timeout)
{
    return SC(NtWaitForSingleObject, H(h), U(alertable), P(timeout));
}

NTSTATUS NTAPI NtWaitForMultipleObjects(ULONG n, const HANDLE *h, WAIT_TYPE type, BOOLEAN alertable,
                                        PLARGE_INTEGER timeout)
{
    U64 hv[64];
    if (n > 64) return STATUS_INVALID_PARAMETER;
    for (ULONG i = 0; i < n; i++) hv[i] = H(h[i]);
    return SC(NtWaitForMultipleObjects, U(n), P(hv), U(type), U(alertable), P(timeout));
}

NTSTATUS NTAPI NtDuplicateObject(HANDLE sp, HANDLE src, HANDLE tp, PHANDLE dst, ULONG access,
                                 ULONG attrs, ULONG options)
{
    Box hb;
    NTSTATUS s = SC(NtDuplicateObject, H(sp), H(src), H(tp), HBOX(hb, dst), U(access), U(attrs), U(options));
    box_out(&hb);
    return s;
}

NTSTATUS NTAPI NtCompareObjects(HANDLE a, HANDLE b) { return SC(NtCompareObjects, H(a), H(b)); }

/* -----------------------------------------------------------------------
 * Registry
 * ----------------------------------------------------------------------- */
NTSTATUS NTAPI NtCreateKey(PHANDLE key, ACCESS_MASK access, POBJECT_ATTRIBUTES oa, ULONG title, PUNICODE_STRING cls,
                           ULONG options, PULONG disposition)
{
    Box hb; OAC oc; US64 c;
    NTSTATUS s = SC(NtCreateKey, HBOX(hb, key), U(access), oa_in(&oc, oa), U(title), us_in(&c, cls), U(options),
                    P(disposition));
    box_out(&hb);
    return s;
}

NTSTATUS NTAPI NtOpenKeyEx(PHANDLE key, ACCESS_MASK access, POBJECT_ATTRIBUTES oa, ULONG options)
{
    Box hb; OAC oc;
    NTSTATUS s = SC(NtOpenKeyEx, HBOX(hb, key), U(access), oa_in(&oc, oa), U(options));
    box_out(&hb);
    return s;
}

NTSTATUS NTAPI NtDeleteKey(HANDLE key) { return SC(NtDeleteKey, H(key)); }
NTSTATUS NTAPI NtFlushKey(HANDLE key)  { return SC(NtFlushKey, H(key)); }
NTSTATUS NTAPI NtShutdownSystem(ULONG action) { return SC(NtShutdownSystem, U(action)); }
NTSTATUS NTAPI NtSetSystemPowerState(ULONG action, ULONG min_state, ULONG flags)
{ return SC(NtSetSystemPowerState, U(action), U(min_state), U(flags)); }
NTSTATUS NTAPI NtInitiatePowerAction(ULONG action, ULONG min_state, ULONG flags, BOOLEAN async)
{ return SC(NtInitiatePowerAction, U(action), U(min_state), U(flags), U(async)); }
NTSTATUS NTAPI NtPowerInformation(ULONG level, PVOID in, ULONG inlen, PVOID out, ULONG outlen)
{ return SC(NtPowerInformation, U(level), P(in), U(inlen), P(out), U(outlen)); }

NTSTATUS NTAPI NtSetValueKey(HANDLE key, PUNICODE_STRING name, ULONG title, ULONG type, PVOID data, ULONG size)
{
    US64 n;
    return SC(NtSetValueKey, H(key), us_in(&n, name), U(title), U(type), P(data), U(size));
}

NTSTATUS NTAPI NtQueryValueKey(HANDLE key, PUNICODE_STRING name, int cls, PVOID info, ULONG len, PULONG ret)
{
    US64 n;
    return SC(NtQueryValueKey, H(key), us_in(&n, name), U(cls), P(info), U(len), P(ret));
}

NTSTATUS NTAPI NtEnumerateValueKey(HANDLE key, ULONG index, int cls, PVOID info, ULONG len, PULONG ret)
{
    return SC(NtEnumerateValueKey, H(key), U(index), U(cls), P(info), U(len), P(ret));
}

NTSTATUS NTAPI NtDeleteValueKey(HANDLE key, PUNICODE_STRING name)
{
    US64 n;
    return SC(NtDeleteValueKey, H(key), us_in(&n, name));
}

NTSTATUS NTAPI NtEnumerateKey(HANDLE key, ULONG index, int cls, PVOID info, ULONG len, PULONG ret)
{
    return SC(NtEnumerateKey, H(key), U(index), U(cls), P(info), U(len), P(ret));
}

NTSTATUS NTAPI NtQueryKey(HANDLE key, int cls, PVOID info, ULONG len, PULONG ret)
{
    return SC(NtQueryKey, H(key), U(cls), P(info), U(len), P(ret));
}

NTSTATUS NTAPI NtRenameKey(HANDLE key, PUNICODE_STRING name)
{
    US64 n;
    return SC(NtRenameKey, H(key), us_in(&n, name));
}

/* -----------------------------------------------------------------------
 * Time
 * ----------------------------------------------------------------------- */
NTSTATUS NTAPI NtQuerySystemTime(PLARGE_INTEGER t) { return SC(NtQuerySystemTime, P(t)); }
NTSTATUS NTAPI NtQueryPerformanceCounter(PLARGE_INTEGER c, PLARGE_INTEGER f)
{
    return SC(NtQueryPerformanceCounter, P(c), P(f));
}
NTSTATUS NTAPI NtDelayExecution(BOOLEAN alertable, PLARGE_INTEGER interval)
{
    return SC(NtDelayExecution, U(alertable), P(interval));
}
NTSTATUS NTAPI NtYieldExecution(void) { return SC0(NtYieldExecution); }
NTSTATUS NTAPI NtWaitForAlertByThreadId(PVOID address, PLARGE_INTEGER timeout)
{
    return SC(NtWaitForAlertByThreadId, P(address), P(timeout));
}
NTSTATUS NTAPI NtAlertThreadByThreadId(HANDLE tid) { return SC(NtAlertThreadByThreadId, P(tid)); }

/* -----------------------------------------------------------------------
 * NovaOS services
 * ----------------------------------------------------------------------- */
NTSTATUS NTAPI NtNovaLoadDll(const char *name, ULONG len, PVOID *base, ULONG flags)
{
    U64 b = 0;
    NTSTATUS s = SC(NtNovaLoadDll, P(name), U(len), P(&b), U(flags));
    if (base && NT_SUCCESS(s)) *base = (PVOID)(ULONG_PTR)b;
    return s;
}

NTSTATUS NTAPI NtNovaDebugPrint(const char *s, ULONG len) { return SC(NtNovaDebugPrint, P(s), U(len)); }
NTSTATUS NTAPI NtNovaGetRandom(void *buf, ULONG len)      { return SC(NtNovaGetRandom, P(buf), U(len)); }

/* sockets: handles are small numbers; results are counts or -errno */
INT_PTR  NTAPI NtNovaSocket(ULONG type)                          { return SCP(NtNovaSocket, U(type)); }
LONG_PTR NTAPI NtNovaSockConnect(INT_PTR h, ULONG ip, USHORT port) { return SCP(NtNovaSockConnect, S(h), U(ip), U(port)); }
LONG_PTR NTAPI NtNovaSockSend(INT_PTR h, const void *buf, ULONG len) { return SCP(NtNovaSockSend, S(h), P(buf), U(len)); }
LONG_PTR NTAPI NtNovaSockRecv(INT_PTR h, void *buf, ULONG len)   { return SCP(NtNovaSockRecv, S(h), P(buf), U(len)); }
LONG_PTR NTAPI NtNovaSockBind(INT_PTR h, ULONG ip, USHORT port)  { return SCP(NtNovaSockBind, S(h), U(ip), U(port)); }
LONG_PTR NTAPI NtNovaSockListen(INT_PTR h, ULONG backlog)        { return SCP(NtNovaSockListen, S(h), U(backlog)); }
INT_PTR  NTAPI NtNovaSockAccept(INT_PTR h, void *addr)           { return SCP(NtNovaSockAccept, S(h), P(addr)); }
LONG_PTR NTAPI NtNovaSockCtl(INT_PTR h, ULONG op, ULONG_PTR arg, void *out)
{
    return SCP(NtNovaSockCtl, S(h), U(op), P(arg), P(out));
}
LONG_PTR NTAPI NtNovaSockSendTo(INT_PTR h, const void *buf, ULONG len, const void *addr)
{
    return SCP(NtNovaSockSendTo, S(h), P(buf), U(len), P(addr));
}
LONG_PTR NTAPI NtNovaSockRecvFrom(INT_PTR h, void *buf, ULONG len, void *addr)
{
    return SCP(NtNovaSockRecvFrom, S(h), P(buf), U(len), P(addr));
}
LONG_PTR NTAPI NtNovaResolve(const char *name, ULONG *ip)        { return SCP(NtNovaResolve, P(name), P(ip)); }
INT_PTR  NTAPI NtNovaAudioOpen(ULONG frames)                      { return SCP(NtNovaAudioOpen, U(frames)); }
LONG_PTR NTAPI NtNovaAudioWrite(INT_PTR h, const void *frames, ULONG n) { return SCP(NtNovaAudioWrite, S(h), P(frames), U(n)); }
LONG_PTR NTAPI NtNovaAudioCtl(INT_PTR h, ULONG op, ULONG_PTR arg, void *out)
{
    return SCP(NtNovaAudioCtl, S(h), U(op), U(arg), P(out));
}

/* windows: the creation block is laid out with 64-bit fields already */
LONG_PTR NTAPI NtNovaGuiCreate(void *info)                       { return SCP(NtNovaGuiCreate, P(info)); }
LONG_PTR NTAPI NtNovaGuiGetMessage(ULONG_PTR hwnd, void *msg, ULONG wait)
{
    /* the kernel fills a 64-bit MSG */
    struct { U64 hwnd; ULONG message, pad; U64 wParam, lParam; ULONG time; LONG x, y; } k;
    memset(&k, 0, sizeof(k));
    LONG_PTR r = SCP(NtNovaGuiGetMessage, P(hwnd), P(msg ? &k : 0), U(wait));
    if (msg && r == 1) {
        MSG *m = msg;
        m->hwnd = (HWND)(ULONG_PTR)k.hwnd;
        m->message = k.message;
        m->wParam = (WPARAM)k.wParam;
        m->lParam = (LPARAM)k.lParam;
        m->time = k.time;
        m->pt.x = k.x; m->pt.y = k.y;
    }
    return r;
}
LONG_PTR NTAPI NtNovaGuiInvalidate(ULONG_PTR hwnd)               { return SCP(NtNovaGuiInvalidate, P(hwnd)); }
LONG_PTR NTAPI NtNovaGuiSetText(ULONG_PTR hwnd, const void *t)   { return SCP(NtNovaGuiSetText, P(hwnd), P(t)); }
LONG_PTR NTAPI NtNovaGuiShow(ULONG_PTR hwnd, ULONG show)         { return SCP(NtNovaGuiShow, P(hwnd), U(show)); }
LONG_PTR NTAPI NtNovaGuiDestroy(ULONG_PTR hwnd)                  { return SCP(NtNovaGuiDestroy, P(hwnd)); }
LONG_PTR NTAPI NtNovaGuiSetTimer(ULONG_PTR hwnd, ULONG_PTR id, ULONG ms) { return SCP(NtNovaGuiSetTimer, P(hwnd), P(id), U(ms)); }
LONG_PTR NTAPI NtNovaGuiKillTimer(ULONG_PTR hwnd, ULONG_PTR id)  { return SCP(NtNovaGuiKillTimer, P(hwnd), P(id)); }
LONG_PTR NTAPI NtNovaGuiMessageBox(const void *text16, const void *cap16, ULONG type)
{
    return SCP(NtNovaGuiMessageBox, P(text16), P(cap16), U(type));
}
LONG_PTR NTAPI NtNovaGuiScreenSize(ULONG *w, ULONG *h)           { return SCP(NtNovaGuiScreenSize, P(w), P(h)); }
LONG_PTR NTAPI NtNovaGuiPostMessage(ULONG_PTR hwnd, ULONG msg, ULONG_PTR wp, ULONG_PTR lp)
{
    return SCP(NtNovaGuiPostMessage, P(hwnd), U(msg), P(wp), S(lp));
}
LONG_PTR NTAPI NtNovaGuiCtl(ULONG_PTR hwnd, ULONG op, ULONG_PTR arg, PVOID data)
{
    return SCP(NtNovaGuiCtl, P(hwnd), U(op), P(arg), P(data));
}

#endif /* !_WIN64 */
