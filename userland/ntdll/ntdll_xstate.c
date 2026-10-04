#define NOVA_BUILD_NTDLL
/*
 * ntdll_xstate.c — extended contexts: a CONTEXT with room for the
 * processor's extended state (XSAVE) after it
 *
 * Windows lays such a buffer out as the CONTEXT, then a CONTEXT_EX that
 * says where each part is (offsets from the CONTEXT_EX itself), then,
 * with CONTEXT_XSTATE in the flags, a 64-byte-aligned XSAVE area: its
 * 64-byte header and the enabled features past the legacy x87/SSE state
 * (which lives in the CONTEXT's FltSave).  kernel32's InitializeContext,
 * LocateXStateFeature and the like are built on these, and programs that
 * read a thread's full register state call them directly (Roblox's
 * Hyperion does).  NovaOS keeps the legacy state only (KUSER_SHARED_DATA's
 * XState says so too), so the XSAVE area is just its header.
 */

#include <winternl.h>
#include <winnt.h>

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);

typedef struct { LONG Offset; ULONG Length; } CONTEXT_CHUNK;
typedef struct { CONTEXT_CHUNK All, Legacy, XState; } CONTEXT_EX, *PCONTEXT_EX;

#ifdef _WIN64
#define CONTEXT_ARCH    0x00100000u             /* CONTEXT_AMD64 */
#define CONTEXT_ALIGN   16
#else
#define CONTEXT_ARCH    0x00010000u             /* CONTEXT_i386 */
#define CONTEXT_ALIGN   4
#endif
#define CONTEXT_XSTATE_BIT 0x40u
#define XSAVE_HEADER    64                      /* XSAVE_AREA_HEADER: Mask, CompactionMask, Reserved */
#define XSAVE_ALIGN     64
#define LEGACY_FEATURES 3ull                    /* x87 and SSE */

/* The features this machine saves, from KUSER_SHARED_DATA's XState
 * (XSTATE_CONFIGURATION.EnabledFeatures), as on Windows */
#define KUSD_XSTATE_ENABLED ((volatile ULONG64 *)(ULONG_PTR)0x7FFE03D8)
NTSYSAPI ULONG64 NTAPI RtlGetEnabledExtendedFeatures(ULONG64 mask) { return mask & *KUSD_XSTATE_ENABLED; }

static BOOL flags_ok(ULONG flags) { return (flags & ~0xFFu & ~CONTEXT_ARCH) == 0 && (flags & CONTEXT_ARCH); }

/* Bytes after the CONTEXT_EX for the XSAVE area @flags ask for (0: none);
 * the features past the legacy ones would follow the header */
static ULONG xstate_size(ULONG flags) { return (flags & CONTEXT_XSTATE_BIT) ? XSAVE_HEADER : 0; }

NTSYSAPI NTSTATUS NTAPI RtlGetExtendedContextLength2(ULONG flags, PULONG length, ULONG64 compaction)
{
    (void)compaction;
    if (!flags_ok(flags)) return STATUS_INVALID_PARAMETER;
    ULONG n = (CONTEXT_ALIGN - 1) + sizeof(CONTEXT) + sizeof(CONTEXT_EX);
    if (flags & CONTEXT_XSTATE_BIT) n += (XSAVE_ALIGN - 1) + xstate_size(flags);
    *length = n;
    return STATUS_SUCCESS;
}

NTSYSAPI NTSTATUS NTAPI RtlGetExtendedContextLength(ULONG flags, PULONG length)
{
    return RtlGetExtendedContextLength2(flags, length, ~0ull);
}

NTSYSAPI NTSTATUS NTAPI RtlInitializeExtendedContext2(PVOID buffer, ULONG flags, PCONTEXT_EX *out, ULONG64 compaction)
{
    (void)compaction;
    if (!flags_ok(flags)) return STATUS_INVALID_PARAMETER;
    BYTE *c = (BYTE *)(((ULONG_PTR)buffer + CONTEXT_ALIGN - 1) & ~(ULONG_PTR)(CONTEXT_ALIGN - 1));
    ((CONTEXT *)c)->ContextFlags = flags;
    PCONTEXT_EX ex = (PCONTEXT_EX)(c + sizeof(CONTEXT));
    BYTE *end = (BYTE *)(ex + 1);
    ex->Legacy.Offset = -(LONG)sizeof(CONTEXT);
    ex->Legacy.Length = sizeof(CONTEXT);
    if (flags & CONTEXT_XSTATE_BIT) {
        BYTE *xs = (BYTE *)(((ULONG_PTR)end + XSAVE_ALIGN - 1) & ~(ULONG_PTR)(XSAVE_ALIGN - 1));
        memset(xs, 0, XSAVE_HEADER);
        ex->XState.Offset = (LONG)(xs - (BYTE *)ex);
        ex->XState.Length = xstate_size(flags);
        end = xs + ex->XState.Length;
    } else {
        ex->XState.Offset = sizeof(CONTEXT_EX);
        ex->XState.Length = 0;
    }
    ex->All.Offset = -(LONG)sizeof(CONTEXT);
    ex->All.Length = (ULONG)(end - c);
    *out = ex;
    return STATUS_SUCCESS;
}

NTSYSAPI NTSTATUS NTAPI RtlInitializeExtendedContext(PVOID buffer, ULONG flags, PCONTEXT_EX *out)
{
    return RtlInitializeExtendedContext2(buffer, flags, out, ~0ull);
}

NTSYSAPI PCONTEXT NTAPI RtlLocateLegacyContext(PCONTEXT_EX ex, PULONG length)
{
    if (length) *length = ex->Legacy.Length;
    return (PCONTEXT)((BYTE *)ex + ex->Legacy.Offset);
}

/* A feature's place in the XSAVE area: none past the legacy ones here
 * (and those two are in the CONTEXT: RtlLocateLegacyContext) */
NTSYSAPI PVOID NTAPI RtlLocateExtendedFeature2(PCONTEXT_EX ex, ULONG id, PVOID config, PULONG length)
{
    (void)ex; (void)id; (void)config;
    if (length) *length = 0;
    return 0;
}

NTSYSAPI PVOID NTAPI RtlLocateExtendedFeature(PCONTEXT_EX ex, ULONG id, PULONG length)
{
    return RtlLocateExtendedFeature2(ex, id, 0, length);
}

/* XSAVE_AREA_HEADER.Mask: which features the area holds */
NTSYSAPI ULONG64 NTAPI RtlGetExtendedFeaturesMask(PCONTEXT_EX ex)
{
    if (!ex->XState.Length) return 0;
    return *(ULONG64 *)((BYTE *)ex + ex->XState.Offset);
}

NTSYSAPI VOID NTAPI RtlSetExtendedFeaturesMask(PCONTEXT_EX ex, ULONG64 mask)
{
    if (ex->XState.Length) *(ULONG64 *)((BYTE *)ex + ex->XState.Offset) = mask & ~LEGACY_FEATURES;
}

/* Copy the parts @flags name (plus the architecture bit) from one CONTEXT
 * to another */
NTSYSAPI NTSTATUS NTAPI RtlCopyContext(PCONTEXT dst, ULONG flags, PCONTEXT src)
{
    if (!flags_ok(flags)) return STATUS_INVALID_PARAMETER;
    ULONG keep = dst->ContextFlags;
    memcpy(dst, src, sizeof(CONTEXT));
    dst->ContextFlags = (keep & CONTEXT_ARCH) | (flags & src->ContextFlags & ~CONTEXT_XSTATE_BIT);
    return STATUS_SUCCESS;
}

NTSYSAPI NTSTATUS NTAPI RtlCopyExtendedContext(PCONTEXT_EX dst, ULONG flags, PCONTEXT_EX src)
{
    NTSTATUS s = RtlCopyContext(RtlLocateLegacyContext(dst, 0), flags, RtlLocateLegacyContext(src, 0));
    if (NT_SUCCESS(s) && (flags & CONTEXT_XSTATE_BIT) && dst->XState.Length && src->XState.Length)
        memcpy((BYTE *)dst + dst->XState.Offset, (BYTE *)src + src->XState.Offset, XSAVE_HEADER);
    return s;
}
