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
    IrqState irq = irq_save();
    NTSTATUS s = probe(addr, len, align, false);
    irq_restore(irq);
    return s;
}

NTSTATUS ProbeForWrite(void *addr, size_t len, UINT32 align)
{
    IrqState irq = irq_save();
    NTSTATUS s = probe(addr, len, align, true);
    irq_restore(irq);
    return s;
}

/* -----------------------------------------------------------------------
 * Copy helpers — probe and copy with interrupts off (see probe.h)
 * ----------------------------------------------------------------------- */

NTSTATUS CopyFromUser(void *kdst, const void *usrc, size_t len)
{
    IrqState irq = irq_save();
    NTSTATUS s = probe(usrc, len, 1, false);
    if (NT_SUCCESS(s) && len) memcpy(kdst, usrc, len);
    irq_restore(irq);
    return s;
}

NTSTATUS CopyToUser(void *udst, const void *ksrc, size_t len)
{
    IrqState irq = irq_save();
    NTSTATUS s = probe(udst, len, 1, true);
    if (NT_SUCCESS(s) && len) memcpy(udst, ksrc, len);
    irq_restore(irq);
    return s;
}

NTSTATUS CopyUserToUser(void *udst, const void *usrc, size_t len)
{
    IrqState irq = irq_save();
    NTSTATUS s = probe(usrc, len, 1, false);
    if (NT_SUCCESS(s)) s = probe(udst, len, 1, true);
    if (NT_SUCCESS(s) && len) memmove(udst, usrc, len);
    irq_restore(irq);
    return s;
}

NTSTATUS ZeroUser(void *udst, size_t len)
{
    IrqState irq = irq_save();
    NTSTATUS s = probe(udst, len, 1, true);
    if (NT_SUCCESS(s) && len) memset(udst, 0, len);
    irq_restore(irq);
    return s;
}

/* Strings are probed one page at a time, since their length is unknown
 * until the terminator is found. */
NTSTATUS CopyStringFromUser(char *kdst, size_t cap, const char *usrc)
{
    if (!kdst || cap == 0) return STATUS_INVALID_PARAMETER;

    IrqState irq = irq_save();
    NTSTATUS s = STATUS_SUCCESS;
    size_t   n = 0;
    UINT64   checked_end = 0;   /* end of the last page validated */

    while (n < cap - 1) {
        UINT64 a = (UINT64)(uintptr_t)(usrc + n);
        if (a >= checked_end) {
            UINT64 page = a & ~((UINT64)PAGE_SIZE - 1);
            s = probe((const void *)(uintptr_t)page, PAGE_SIZE, 1, false);
            if (!NT_SUCCESS(s)) break;
            checked_end = page + PAGE_SIZE;
        }
        char c = usrc[n];
        if (!c) break;
        kdst[n++] = c;
    }
    kdst[n] = '\0';
    irq_restore(irq);
    return s;
}

NTSTATUS ProbeUserWideStringLength(const WCHAR *usrc, UINT32 max_chars,
                                   UINT32 *len_out)
{
    if (!len_out) return STATUS_INVALID_PARAMETER;
    if ((UINT64)(uintptr_t)usrc & (sizeof(WCHAR) - 1))
        return STATUS_DATATYPE_MISALIGNMENT;

    IrqState irq = irq_save();
    NTSTATUS s = STATUS_INVALID_PARAMETER;   /* no NUL within max_chars */
    UINT64   checked_end = 0;

    for (UINT32 i = 0; i <= max_chars; i++) {
        UINT64 a = (UINT64)(uintptr_t)(usrc + i);
        if (a >= checked_end) {
            UINT64 page = a & ~((UINT64)PAGE_SIZE - 1);
            NTSTATUS ps = probe((const void *)(uintptr_t)page, PAGE_SIZE, 1, false);
            if (!NT_SUCCESS(ps)) { s = ps; break; }
            checked_end = page + PAGE_SIZE;
        }
        if (!usrc[i]) { *len_out = i; s = STATUS_SUCCESS; break; }
    }
    irq_restore(irq);
    return s;
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
