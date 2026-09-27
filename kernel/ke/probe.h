/*
 * probe.h — validation and copying of user-mode pointers
 *
 * Every pointer a system call receives from ring 3 is untrusted: it may
 * point into the kernel half of the address space, be unmapped, or be
 * read-only.  Syscall handlers must never dereference such a pointer
 * directly — they copy the data into a kernel buffer with CopyFromUser()
 * and copy results back with CopyToUser().
 *
 * A range is acceptable only when:
 *   - it lies entirely within [USER_PROBE_MIN, USER_PROBE_LIMIT),
 *   - addr + len does not overflow,
 *   - addr is a multiple of the requested alignment, and
 *   - every page it touches is mapped Present + User (+ Writable for
 *     writes) in the *current* address space.
 *
 * The page-table check and the copy happen together with interrupts
 * disabled.  The int 0x2E gate is a trap gate, so syscalls are otherwise
 * preemptible; doing both in one uninterrupted step (single CPU) means
 * another thread of the same process cannot unmap the range in between.
 *
 * A zero-length range always succeeds and touches nothing.
 */

#pragma once

#include "../include/types.h"
#include "../ob/ob.h"

/* Lowest and one-past-highest user addresses.  The first 64 KiB are never
 * mapped, so NULL and small NULL-relative pointers are always rejected.
 * The limit is the canonical user/kernel split, not USER_ADDRESS_MAX from
 * mm/vma.h (the VMA allocation ceiling): the PEB and TEB are mapped above
 * that ceiling and must still be reachable. */
#define USER_PROBE_MIN    UINT64_C(0x0000000000010000)
#define USER_PROBE_LIMIT  UINT64_C(0x0000800000000000)

/* Largest single transfer the syscall layer bounces through the kernel
 * heap (read/write/query buffers).  Larger requests are clamped; the
 * IO_STATUS_BLOCK reports how many bytes were actually transferred. */
#define USER_MAX_BOUNCE   (64u * 1024u)

/* True if [addr, addr+len) lies inside the user half (no mapping check). */
bool     MmIsUserRange(UINT64 addr, UINT64 len);

/* Validate a user range for reading / writing (see rules above). */
NTSTATUS ProbeForRead(const void *addr, size_t len, UINT32 align);
NTSTATUS ProbeForWrite(void *addr, size_t len, UINT32 align);

/* Probe and copy in one uninterrupted step.  Return STATUS_SUCCESS or
 * STATUS_ACCESS_VIOLATION / STATUS_DATATYPE_MISALIGNMENT. */
NTSTATUS CopyFromUser(void *kdst, const void *usrc, size_t len);
NTSTATUS CopyToUser(void *udst, const void *ksrc, size_t len);

/* User → user copy (memmove semantics) and user zero-fill. */
NTSTATUS CopyUserToUser(void *udst, const void *usrc, size_t len);
NTSTATUS ZeroUser(void *udst, size_t len);

/* Copy a NUL-terminated user string into kdst[cap], always terminated.
 * Stops at the first NUL or after cap-1 bytes. */
NTSTATUS CopyStringFromUser(char *kdst, size_t cap, const char *usrc);

/* Count the WCHARs before the terminating NUL of a user wide string,
 * reading at most max_chars.  Fails if no NUL is found within the limit. */
NTSTATUS ProbeUserWideStringLength(const WCHAR *usrc, UINT32 max_chars,
                                   UINT32 *len_out);

/* -----------------------------------------------------------------------
 * Capturing NT structures
 *
 * The captured copy lives entirely in kernel memory and may be passed to
 * cm/io/section code in place of the user pointer.  Release it with the
 * matching Release call on every path.
 * ----------------------------------------------------------------------- */

/* Copy a user UNICODE_STRING and its buffer.  The kernel buffer is
 * Length + sizeof(WCHAR) bytes and always NUL-terminated (which also
 * terminates the ASCII-in-Buffer convention used by io.c). */
NTSTATUS CaptureUnicodeString(const void *user_us, UNICODE_STRING *out);
void     ReleaseCapturedUnicodeString(UNICODE_STRING *us);

typedef struct _CAPTURED_OBJECT_ATTRIBUTES {
    OBJECT_ATTRIBUTES Attributes;   /* ObjectName → &Name (or NULL) */
    UNICODE_STRING    Name;
} CAPTURED_OBJECT_ATTRIBUTES;

/* Copy a user OBJECT_ATTRIBUTES (and its ObjectName).  Security fields
 * are dropped and kernel-only attribute flags (OBJ_KERNEL_HANDLE,
 * OBJ_PERMANENT) are stripped, as NT does for user-mode callers. */
NTSTATUS CaptureObjectAttributes(const void *user_oa,
                                 CAPTURED_OBJECT_ATTRIBUTES *out);
void     ReleaseCapturedObjectAttributes(CAPTURED_OBJECT_ATTRIBUTES *coa);

/* Boot-time self-test of the rules above (probe_test.c) */
void     KiProbeSelfTest(void);
