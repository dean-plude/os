/*
 * probe.c — validation and copying of user-mode pointers
 *
 * See probe.h for the rules.  The mapping check walks the page tables of
 * the *current* CR3 (the address space the CPU would use for the access)
 * and requires Present + User — and Writable for writes — at every level,
 * exactly as the MMU does for a ring-3 access.
 */

#include "probe.h"
#include "../lib/string.h"
#include "../mm/vmm.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/paging.h"
#include "../arch/x86_64/idt.h"

#define PROBE_PAGE_1GB  (1024UL * 1024 * 1024)

/* Kernel-only attribute flags a user caller may not request. */
#define OBJ_USER_INVALID_ATTRIBUTES  (OBJ_KERNEL_HANDLE | OBJ_PERMANENT)

/* -----------------------------------------------------------------------
 * Range + page-table checks
 * ----------------------------------------------------------------------- */

bool MmIsUserRange(UINT64 addr, UINT64 len)
{
    if (addr < USER_PROBE_MIN) return false;
    if (len > USER_PROBE_LIMIT - addr) return false;   /* also catches overflow */
    return addr < USER_PROBE_LIMIT;
}

static const pte_t *table_at(pte_t entry)
{
    return (const pte_t *)(PHYSMAP_BASE + (entry & PTE_ADDR_MASK));
}

/* Every page of [addr, addr+len) is mapped with at least `need` bits at
 * every paging level of the current address space.  Caller has already
 * validated the range, so addr + len cannot overflow. */
static bool user_pages_ok(UINT64 addr, UINT64 len, UINT64 need)
{
    UINT64 va  = addr & ~((UINT64)PAGE_SIZE - 1);
    UINT64 end = addr + len;
    pte_t  cr3 = (pte_t)read_cr3();
    const pte_t *pml4 = table_at(cr3);

    while (va < end) {
        pte_t e = pml4[PML4_IDX(va)];
        if ((e & need) != need) return false;

        e = table_at(e)[PDPT_IDX(va)];
        if ((e & need) != need) return false;
        if (e & PTE_HUGE) { va = (va | (PROBE_PAGE_1GB - 1)) + 1; continue; }

        e = table_at(e)[PD_IDX(va)];
        if ((e & need) != need) return false;
        if (e & PTE_HUGE) { va = (va | (HUGE_PAGE_SIZE - 1)) + 1; continue; }

        e = table_at(e)[PT_IDX(va)];
        if ((e & need) != need) return false;
        va += PAGE_SIZE;
    }
    return true;
}

static NTSTATUS probe(const void *addr, size_t len, UINT32 align, bool write)
{
    if (len == 0) return STATUS_SUCCESS;

    UINT64 a = (UINT64)(uintptr_t)addr;
    if (align > 1 && (a & (align - 1)))
        return STATUS_DATATYPE_MISALIGNMENT;
    if (!MmIsUserRange(a, len))
        return STATUS_ACCESS_VIOLATION;

    UINT64 need = PTE_PRESENT | PTE_USER | (write ? PTE_WRITE : 0);
    return user_pages_ok(a, len, need) ? STATUS_SUCCESS
                                       : STATUS_ACCESS_VIOLATION;
}

NTSTATUS ProbeForRead(const void *addr, size_t len, UINT32 align)
{
    return probe(addr, len, align, false);
}

NTSTATUS ProbeForWrite(void *addr, size_t len, UINT32 align)
{
    return probe(addr, len, align, true);
}

/* -----------------------------------------------------------------------
 * Copy helpers.  The probe checks the range and the page tables; the copy
 * itself (uaccess.asm) survives the memory going away meanwhile — another
 * thread of the program may unmap it from another CPU — and then reports
 * an access violation.
 * ----------------------------------------------------------------------- */
extern int  uaccess_copy(void *dst, const void *src, size_t len);
extern int  uaccess_zero(void *dst, size_t len);
extern char uaccess_begin[], uaccess_end[], uaccess_fault[];

bool UserCopyFixup(InterruptFrame *f)
{
    if (f->rip < (UINT64)(uintptr_t)uaccess_begin || f->rip >= (UINT64)(uintptr_t)uaccess_end)
        return false;
    f->rip = (UINT64)(uintptr_t)uaccess_fault;
    return true;
}

NTSTATUS CopyFromUser(void *kdst, const void *usrc, size_t len)
{
    NTSTATUS s = probe(usrc, len, 1, false);
    if (NT_SUCCESS(s) && len && uaccess_copy(kdst, usrc, len)) s = STATUS_ACCESS_VIOLATION;
    return s;
}

NTSTATUS CopyToUser(void *udst, const void *ksrc, size_t len)
{
    NTSTATUS s = probe(udst, len, 1, true);
    if (NT_SUCCESS(s) && len && uaccess_copy(udst, ksrc, len)) s = STATUS_ACCESS_VIOLATION;
    return s;
}

NTSTATUS CopyUserToUser(void *udst, const void *usrc, size_t len)
{
    NTSTATUS s = probe(usrc, len, 1, false);
    if (NT_SUCCESS(s)) s = probe(udst, len, 1, true);
    if (NT_SUCCESS(s) && len && uaccess_copy(udst, usrc, len)) s = STATUS_ACCESS_VIOLATION;
    return s;
}

NTSTATUS ZeroUser(void *udst, size_t len)
{
    NTSTATUS s = probe(udst, len, 1, true);
    if (NT_SUCCESS(s) && len && uaccess_zero(udst, len)) s = STATUS_ACCESS_VIOLATION;
    return s;
}

/* Strings are copied a page at a time, since their length is unknown
 * until the terminator is found. */
NTSTATUS CopyStringFromUser(char *kdst, size_t cap, const char *usrc)
{
    if (!kdst || cap == 0) return STATUS_INVALID_PARAMETER;

    NTSTATUS s = STATUS_SUCCESS;
    size_t   n = 0;
    while (n < cap - 1) {
        UINT64 a = (UINT64)(uintptr_t)(usrc + n);
        size_t chunk = PAGE_SIZE - (a & (PAGE_SIZE - 1));
        if (chunk > cap - 1 - n) chunk = cap - 1 - n;
        s = probe(usrc + n, chunk, 1, false);
        if (!NT_SUCCESS(s)) break;
        if (uaccess_copy(kdst + n, usrc + n, chunk)) { s = STATUS_ACCESS_VIOLATION; break; }
        size_t i = 0;
        while (i < chunk && kdst[n + i]) i++;
        n += i;
        if (i < chunk) break;                           /* found the terminator */
    }
    kdst[n] = '\0';
    return s;
}

NTSTATUS ProbeUserWideStringLength(const WCHAR *usrc, UINT32 max_chars,
                                   UINT32 *len_out)
{
    if (!len_out) return STATUS_INVALID_PARAMETER;
    if ((UINT64)(uintptr_t)usrc & (sizeof(WCHAR) - 1))
        return STATUS_DATATYPE_MISALIGNMENT;

    WCHAR buf[256];
    for (UINT32 i = 0; i <= max_chars;) {
        UINT64 a = (UINT64)(uintptr_t)(usrc + i);
        UINT32 chunk = (UINT32)((PAGE_SIZE - (a & (PAGE_SIZE - 1))) / sizeof(WCHAR));
        if (chunk > 256) chunk = 256;
        if (chunk > max_chars + 1 - i) chunk = max_chars + 1 - i;
        NTSTATUS ps = probe(usrc + i, chunk * sizeof(WCHAR), 1, false);
        if (!NT_SUCCESS(ps)) return ps;
        if (uaccess_copy(buf, usrc + i, chunk * sizeof(WCHAR))) return STATUS_ACCESS_VIOLATION;
        for (UINT32 k = 0; k < chunk; k++)
            if (!buf[k]) { *len_out = i + k; return STATUS_SUCCESS; }
        i += chunk;
    }
    return STATUS_INVALID_PARAMETER;                    /* no NUL within max_chars */
}

/* -----------------------------------------------------------------------
 * Structure capture
 * ----------------------------------------------------------------------- */

NTSTATUS CaptureUnicodeString(const void *user_us, UNICODE_STRING *out)
{
    if (!out) return STATUS_INVALID_PARAMETER;
    out->Length = out->MaximumLength = 0;
    out->Buffer = NULL;

    UNICODE_STRING us;
    NTSTATUS s = CopyFromUser(&us, user_us, sizeof(us));
    if (!NT_SUCCESS(s)) return s;

    if ((us.Length & 1) || us.Length > us.MaximumLength)
        return STATUS_INVALID_PARAMETER;

    /* Always allocate room for a terminator so consumers that scan for
     * NUL (including io.c's ASCII-in-Buffer convention) stay in bounds. */
    size_t alloc = (size_t)us.Length + sizeof(WCHAR);
    WCHAR *buf = kmalloc(alloc);
    if (!buf) return STATUS_NO_MEMORY;

    if (us.Length) {
        s = CopyFromUser(buf, us.Buffer, us.Length);
        if (!NT_SUCCESS(s)) { kfree(buf); return s; }
    }
    buf[us.Length / sizeof(WCHAR)] = 0;

    out->Buffer        = buf;
    out->Length        = us.Length;
    out->MaximumLength = (USHORT)alloc;
    return STATUS_SUCCESS;
}

void ReleaseCapturedUnicodeString(UNICODE_STRING *us)
{
    if (!us) return;
    kfree(us->Buffer);
    us->Buffer = NULL;
    us->Length = us->MaximumLength = 0;
}

NTSTATUS CaptureObjectAttributes(const void *user_oa,
                                 CAPTURED_OBJECT_ATTRIBUTES *out)
{
    if (!out) return STATUS_INVALID_PARAMETER;
    memset(out, 0, sizeof(*out));

    OBJECT_ATTRIBUTES oa;
    NTSTATUS s = CopyFromUser(&oa, user_oa, sizeof(oa));
    if (!NT_SUCCESS(s)) return s;

    if (oa.ObjectName) {
        s = CaptureUnicodeString(oa.ObjectName, &out->Name);
        if (!NT_SUCCESS(s)) return s;
    }

    out->Attributes.Length        = sizeof(OBJECT_ATTRIBUTES);
    out->Attributes.RootDirectory = oa.RootDirectory;
    out->Attributes.ObjectName    = oa.ObjectName ? &out->Name : NULL;
    out->Attributes.Attributes    = oa.Attributes & OBJ_VALID_ATTRIBUTES
                                    & ~OBJ_USER_INVALID_ATTRIBUTES;
    /* SecurityDescriptor / SecurityQualityOfService are not honoured yet;
     * drop the user pointers rather than pass them on. */
    return STATUS_SUCCESS;
}

void ReleaseCapturedObjectAttributes(CAPTURED_OBJECT_ATTRIBUTES *coa)
{
    if (!coa) return;
    ReleaseCapturedUnicodeString(&coa->Name);
    coa->Attributes.ObjectName = NULL;
}
