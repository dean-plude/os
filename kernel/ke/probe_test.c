/*
 * probe_test.c — boot-time self-test for user-pointer validation
 *
 * No user process runs at boot yet, so this test builds a small user
 * mapping in the current address space, drives the probe helpers and a
 * few syscall handlers (through the real dispatcher) with good and bad
 * pointers, then removes the mapping again.
 *
 * Layout at PROBE_TEST_VA (a PML4 slot that must be empty):
 *   page 0 — user, read/write
 *   page 1 — user, read-only
 *   page 2 — not mapped
 *
 * Only syscalls that need no current process are exercised here: the boot
 * context is not an ETHREAD, so anything that resolves the current
 * process or its handle table would read garbage.
 */

#include "probe.h"
#include "printf.h"
#include "syscall.h"
#include "../lib/string.h"
#include "../mm/pmm.h"
#include "../mm/vma.h"
#include "../ldr/user_stubs.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/paging.h"

#define PROBE_TEST_VA  UINT64_C(0x00006FFF00000000)   /* PML4 index 223 */

static int g_pass, g_fail;

#define CHECK(cond, what) do {                                   \
    if (cond) g_pass++;                                          \
    else { g_fail++; kprintf("[PROBE] FAIL: %s\n", (what)); }    \
} while (0)

static pte_t *tbl(pte_t e) { return (pte_t *)(PHYSMAP_BASE + (e & PTE_ADDR_MASK)); }

/* A kernel object the tests try (and must fail) to reach from "user" */
static volatile UINT64 g_kernel_secret = UINT64_C(0x5EC12E7C0FFEE123);

static UINT64 sys(UINT64 num, UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    return KiSystemCallDispatch(num, a1, a2, a3, a4);
}

static bool is_status(UINT64 r, NTSTATUS s) { return (NTSTATUS)(UINT32)r == s; }

/* -----------------------------------------------------------------------
 * Mapping setup / teardown
 * ----------------------------------------------------------------------- */
static uintptr_t g_data[2];

static bool map_test_pages(void)
{
    uintptr_t cr3 = read_cr3() & PTE_ADDR_MASK;
    pte_t *pml4 = (pte_t *)(PHYSMAP_BASE + cr3);
    if (pml4[PML4_IDX(PROBE_TEST_VA)] & PTE_PRESENT) {
        kprintf("[PROBE] self-test skipped: test PML4 slot in use\n");
        return false;
    }

    MapFlags fl[2] = { MAP_USER | MAP_WRITABLE, MAP_USER };
    for (int i = 0; i < 2; i++) {
        g_data[i] = pmm_alloc_page();
        if (!g_data[i]) return false;
        memset((void *)(PHYSMAP_BASE + g_data[i]), 0, PAGE_SIZE);
        if (!NT_SUCCESS(paging_map_in_pt(cr3, PROBE_TEST_VA + i * PAGE_SIZE,
                                         g_data[i], fl[i])))
            return false;
    }

    /* paging_map_in_pt creates intermediate tables without the U bit;
     * a real ring-3 mapping needs it at every level. */
    pte_t *e = &pml4[PML4_IDX(PROBE_TEST_VA)];
    *e |= PTE_USER;
    e = &tbl(*e)[PDPT_IDX(PROBE_TEST_VA)];
    *e |= PTE_USER;
    e = &tbl(*e)[PD_IDX(PROBE_TEST_VA)];
    *e |= PTE_USER;

    write_cr3(read_cr3());   /* flush */
    return true;
}

static void unmap_test_pages(void)
{
    uintptr_t cr3 = read_cr3() & PTE_ADDR_MASK;
    pte_t *pml4 = (pte_t *)(PHYSMAP_BASE + cr3);
    pte_t  l4 = pml4[PML4_IDX(PROBE_TEST_VA)];
    if (l4 & PTE_PRESENT) {
        pte_t l3 = tbl(l4)[PDPT_IDX(PROBE_TEST_VA)];
        pte_t l2 = tbl(l3)[PD_IDX(PROBE_TEST_VA)];
        pml4[PML4_IDX(PROBE_TEST_VA)] = 0;
        write_cr3(read_cr3());
        pmm_free_page(l2 & PTE_ADDR_MASK);   /* PT   */
        pmm_free_page(l3 & PTE_ADDR_MASK);   /* PD   */
        pmm_free_page(l4 & PTE_ADDR_MASK);   /* PDPT */
    }
    for (int i = 0; i < 2; i++)
        if (g_data[i]) { pmm_free_page(g_data[i]); g_data[i] = 0; }
}

/* -----------------------------------------------------------------------
 * Tests
 * ----------------------------------------------------------------------- */
static void test_probe_rules(void)
{
    void *rw = (void *)(uintptr_t)PROBE_TEST_VA;
    void *ro = (void *)(uintptr_t)(PROBE_TEST_VA + PAGE_SIZE);
    UINT64 kaddr = (UINT64)(uintptr_t)&g_kernel_secret;

    CHECK(ProbeForRead(NULL, 4, 1) == STATUS_ACCESS_VIOLATION, "NULL rejected");
    CHECK(ProbeForRead((void *)(uintptr_t)kaddr, 8, 1) == STATUS_ACCESS_VIOLATION,
          "kernel address rejected");
    CHECK(ProbeForRead(rw, 0, 1) == STATUS_SUCCESS, "zero length accepted");
    CHECK(ProbeForRead((char *)rw + 1, 4, 4) == STATUS_DATATYPE_MISALIGNMENT,
          "misalignment rejected");
    CHECK(ProbeForRead((void *)(uintptr_t)(USER_PROBE_LIMIT - PAGE_SIZE),
                       2 * PAGE_SIZE, 1) == STATUS_ACCESS_VIOLATION,
          "range crossing the user/kernel split rejected");
    CHECK(ProbeForRead(rw, (size_t)~UINT64_C(0), 1) == STATUS_ACCESS_VIOLATION,
          "addr + len overflow rejected");
    CHECK(ProbeForRead(rw, 2 * PAGE_SIZE, 8) == STATUS_SUCCESS,
          "mapped user pages readable");
    CHECK(ProbeForWrite(rw, PAGE_SIZE, 8) == STATUS_SUCCESS,
          "writable user page writable");
    CHECK(ProbeForWrite(ro, 8, 8) == STATUS_ACCESS_VIOLATION,
          "read-only user page not writable");
    CHECK(ProbeForRead((char *)ro + PAGE_SIZE - 4, 8, 1) == STATUS_ACCESS_VIOLATION,
          "range running into an unmapped page rejected");
    CHECK(ProbeForRead((void *)(uintptr_t)(PROBE_TEST_VA + 2 * PAGE_SIZE), 1, 1)
          == STATUS_ACCESS_VIOLATION, "unmapped user page rejected");

    char s[8];
    memcpy(rw, "hello", 6);
    CHECK(CopyStringFromUser(s, sizeof(s), rw) == STATUS_SUCCESS && !strcmp(s, "hello"),
          "user string captured");
    CHECK(CopyStringFromUser(s, sizeof(s), (const char *)(uintptr_t)kaddr)
          == STATUS_ACCESS_VIOLATION && s[0] == '\0',
          "kernel string rejected");
}

static void test_capture(void)
{
    /* Build OBJECT_ATTRIBUTES → UNICODE_STRING → L"Ab" in the user page */
    UINT8 *u = (UINT8 *)(uintptr_t)PROBE_TEST_VA;
    memset(u, 0, 0x200);
    WCHAR *name = (WCHAR *)(u + 0x100);
    name[0] = L'A'; name[1] = L'b';
    UNICODE_STRING    *us = (UNICODE_STRING *)(u + 0x80);
    us->Length = 4; us->MaximumLength = 4; us->Buffer = name;
    OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)u;
    oa->Length     = sizeof(*oa);
    oa->ObjectName = us;
    oa->Attributes = OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE | OBJ_PERMANENT;
    oa->SecurityDescriptor = (void *)(uintptr_t)0xDEADBEEF;

    CAPTURED_OBJECT_ATTRIBUTES coa;
    NTSTATUS s = CaptureObjectAttributes(oa, &coa);
    CHECK(s == STATUS_SUCCESS, "OBJECT_ATTRIBUTES captured");
    if (NT_SUCCESS(s)) {
        UNICODE_STRING *n = coa.Attributes.ObjectName;
        CHECK(n == &coa.Name && (UINT64)(uintptr_t)n->Buffer >= PHYSMAP_BASE,
              "captured name lives in kernel memory");
        CHECK(n->Length == 4 && n->Buffer[0] == L'A' && n->Buffer[1] == L'b' &&
              n->Buffer[2] == 0, "captured name copied and NUL-terminated");
        CHECK(coa.Attributes.Attributes == OBJ_CASE_INSENSITIVE,
              "kernel-only attribute flags stripped");
        CHECK(coa.Attributes.SecurityDescriptor == NULL,
              "security descriptor pointer dropped");
        ReleaseCapturedObjectAttributes(&coa);
    }

    /* Name buffer pointing into the kernel must be refused */
    us->Buffer = (WCHAR *)(uintptr_t)&g_kernel_secret;
    CHECK(CaptureObjectAttributes(oa, &coa) == STATUS_ACCESS_VIOLATION,
          "kernel name buffer rejected");
    us->Buffer = name;
    us->Length = 3;   /* odd byte length */
    CHECK(CaptureObjectAttributes(oa, &coa) == STATUS_INVALID_PARAMETER,
          "odd name length rejected");
}

static void test_syscalls(void)
{
    UINT64 kaddr = (UINT64)(uintptr_t)&g_kernel_secret;
    UINT64 before = g_kernel_secret;
    UINT64 *u = (UINT64 *)(uintptr_t)PROBE_TEST_VA;

    /* Writes aimed at kernel memory are refused and leave it intact */
    CHECK(is_status(sys(SYSCALL_NtQuerySystemTime, kaddr, 0, 0, 0),
                    STATUS_ACCESS_VIOLATION) && g_kernel_secret == before,
          "NtQuerySystemTime(kernel ptr) refused");
    sys(KH_RtlZeroMemory, kaddr, sizeof(UINT64), 0, 0);
    CHECK(g_kernel_secret == before, "RtlZeroMemory(kernel ptr) had no effect");
    CHECK(is_status(sys(SYSCALL_NtQuerySystemInformation, SystemBasicInformation,
                        kaddr, sizeof(SYSTEM_BASIC_INFORMATION), 0),
                    STATUS_ACCESS_VIOLATION) && g_kernel_secret == before,
          "NtQuerySystemInformation(kernel ptr) refused");

    /* Reads of kernel memory into user memory are refused */
    u[0] = 0;
    sys(KH_RtlMoveMemory, PROBE_TEST_VA, kaddr, sizeof(UINT64), 0);
    CHECK(u[0] == 0, "RtlMoveMemory(kernel src) leaked nothing");

    /* Capture failure happens before any handle is created */
    CHECK(is_status(sys(SYSCALL_NtOpenKey, PROBE_TEST_VA, 0, kaddr, 0),
                    STATUS_ACCESS_VIOLATION), "NtOpenKey(kernel OBJECT_ATTRIBUTES) refused");

    /* Good pointers still work */
    u[0] = ~UINT64_C(0);
    CHECK(is_status(sys(SYSCALL_NtQuerySystemTime, PROBE_TEST_VA, 0, 0, 0),
                    STATUS_SUCCESS) && u[0] == 0, "NtQuerySystemTime(user ptr) works");
    u[0] = UINT64_C(0x1122334455667788); u[1] = 0;
    sys(KH_RtlMoveMemory, PROBE_TEST_VA + 8, PROBE_TEST_VA, 8, 0);
    CHECK(u[1] == UINT64_C(0x1122334455667788), "RtlMoveMemory(user→user) works");
    CHECK(is_status(sys(SYSCALL_NtQuerySystemInformation, SystemBasicInformation,
                        PROBE_TEST_VA, sizeof(SYSTEM_BASIC_INFORMATION), 0),
                    STATUS_SUCCESS) &&
          ((SYSTEM_BASIC_INFORMATION *)u)->PageSize == PAGE_SIZE,
          "NtQuerySystemInformation(user ptr) works");

    /* RtlInitUnicodeString: src L"Hi" at +0x200, dest at +0x100 */
    WCHAR *src = (WCHAR *)(uintptr_t)(PROBE_TEST_VA + 0x200);
    src[0] = L'H'; src[1] = L'i'; src[2] = 0;
    UNICODE_STRING *dst = (UNICODE_STRING *)(uintptr_t)(PROBE_TEST_VA + 0x100);
    memset(dst, 0, sizeof(*dst));
    sys(KH_RtlInitUnicodeString, (UINT64)(uintptr_t)dst, (UINT64)(uintptr_t)src, 0, 0);
    CHECK(dst->Length == 4 && dst->MaximumLength == 6 && dst->Buffer == src,
          "RtlInitUnicodeString(user) works");

    /* Writes to a read-only user page are refused */
    CHECK(is_status(sys(SYSCALL_NtQuerySystemTime, PROBE_TEST_VA + PAGE_SIZE, 0, 0, 0),
                    STATUS_ACCESS_VIOLATION), "NtQuerySystemTime(read-only page) refused");
}

static void test_vma_ranges(void)
{
    VMA_SPACE space;
    VmaInitSpace(&space);

    UINT64 base = USER_ADDRESS_MIN, size = ~UINT64_C(0) - 0xFFF;
    CHECK(VmaAllocate(&space, &base, &size, MEM_RESERVE, PAGE_READWRITE)
          == STATUS_INVALID_PARAMETER, "VmaAllocate huge size rejected");
    /* Size alone fits under the ceiling, but base + size runs past it */
    base = UINT64_C(0x7FFF00000000); size = UINT64_C(0x100000000000);
    CHECK(VmaAllocate(&space, &base, &size, MEM_RESERVE, PAGE_READWRITE)
          == STATUS_INVALID_PARAMETER, "VmaAllocate range past user space rejected");
    base = PHYSMAP_BASE; size = PAGE_SIZE;
    CHECK(VmaAllocate(&space, &base, &size, MEM_RESERVE, PAGE_READWRITE)
          == STATUS_INVALID_PARAMETER, "VmaAllocate kernel base rejected");

    VMA_ENTRY e;
    memset(&e, 0, sizeof(e));
    e.BaseAddress = PHYSMAP_BASE;
    e.RegionSize  = PAGE_SIZE;
    CHECK(VmaMap(&space, &e) == STATUS_INVALID_PARAMETER,
          "VmaMap kernel range rejected");
}

void KiProbeSelfTest(void)
{
    g_pass = g_fail = 0;
    IrqState irq = irq_save();

    if (map_test_pages()) {
        test_probe_rules();
        test_capture();
        test_syscalls();
    }
    unmap_test_pages();
    test_vma_ranges();

    irq_restore(irq);
    kprintf("[PROBE] User-pointer self-test: %d passed, %d failed\n",
            g_pass, g_fail);
}
