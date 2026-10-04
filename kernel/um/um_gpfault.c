/*
 * um_gpfault.c — a general-protection fault in a program, as Windows sees it
 *
 * A #GP has many causes, and Windows' kernel looks at the instruction that
 * raised it before it reports anything (KiPreprocessFault):
 *
 *   - a privileged instruction (CLI, STI, HLT, IN/OUT, INS/OUTS, LGDT,
 *     LIDT, LLDT, LTR, LMSW, INVLPG, MOV to or from a control or debug
 *     register, RDMSR/WRMSR, INVD, WBINVD, CLTS, SYSEXIT, SYSRET) is
 *     reported as STATUS_PRIVILEGED_INSTRUCTION, with no parameters;
 *   - RSM is STATUS_ILLEGAL_INSTRUCTION;
 *   - a MOVDQA (66 0F 6F/7F) or MOVAPS/MOVAPD (0F 28/29) given a memory
 *     operand that is not 16-byte aligned, in a 64-bit thread with
 *     alignment-fault fixup on (NtSetInformationThread
 *     ThreadEnableAlignmentFaultFixup, NtSetInformationProcess
 *     ProcessEnableAlignmentFaultFixup, or SetErrorMode
 *     SEM_NOALIGNMENTFAULTEXCEPT), is fixed up instead of raised: the
 *     instruction is rewritten in the program's code to the unaligned
 *     MOVDQU (F3 0F 6F/7F) or MOVUPS/MOVUPD (0F 10/11) and executed again.
 *     Programs rely on that (Roblox's Hyperion checks for it);
 *   - anything else is an access violation reading address -1.
 *
 * This is that, decoded from the instruction bytes.  NovaOS's own code.
 */

#include "um_internal.h"
#include "../ke/probe.h"
#include "../lib/string.h"

#define ST_PRIVILEGED_INSTRUCTION 0xC0000096u
#define ST_ILLEGAL_INSTRUCTION    0xC000001Du

static bool legacy_prefix(UINT8 b)
{
    return b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 ||
           b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65;
}

/* Is the one-byte opcode @op privileged at CPL 3 (IOPL 0)? */
static bool priv_one(UINT8 op)
{
    return op == 0xFA || op == 0xFB || op == 0xF4 ||           /* CLI, STI, HLT */
           (op >= 0xE4 && op <= 0xE7) ||                       /* IN, OUT imm8 */
           (op >= 0xEC && op <= 0xEF) ||                       /* IN, OUT dx */
           (op >= 0x6C && op <= 0x6F);                         /* INS, OUTS */
}

/* ... and the two-byte 0F @op, @reg the ModRM reg field */
static bool priv_two(UINT8 op, UINT8 reg)
{
    switch (op) {
    case 0x00: return reg == 2 || reg == 3;                    /* LLDT, LTR */
    case 0x01: return reg == 2 || reg == 3 || reg >= 6;        /* LGDT, LIDT, LMSW, INVLPG/SWAPGS */
    case 0x06: case 0x07: case 0x08: case 0x09: case 0x35:     /* CLTS, SYSRET, INVD, WBINVD, SYSEXIT */
        return true;
    default:
        return (op >= 0x20 && op <= 0x23) ||                   /* MOV to/from CRn, DRn */
               (op >= 0x30 && op <= 0x33);                     /* WRMSR, RDTSC, RDMSR, RDPMC */
    }
}

int UmGpFault(UINT64 rip, UINT32 *code, UINT64 *nparams)
{
    UmProcess *p = UmCurrent();
    UmThread *t = UmCurrentThread();
    if (!p || !t) return 0;
    UINT8 b[15];
    UINT32 n = 0;
    /* the instruction, up to its 15-byte limit (or the end of what is readable) */
    while (n < sizeof(b) && NT_SUCCESS(CopyFromUser(&b[n], (const void *)(uintptr_t)(rip + n), 1))) n++;
    UINT32 i = 0;
    int op66 = -1;
    bool rep = false;
    for (; i < n && legacy_prefix(b[i]); i++) {
        if (b[i] == 0x66) op66 = (int)i;
        if (b[i] == 0xF2 || b[i] == 0xF3) rep = true;
    }
    if (!p->wow && i < n && (b[i] & 0xF0) == 0x40) i++;        /* REX (a 32-bit program's INC/DEC) */
    if (i >= n) return 0;
    UINT8 op = b[i++];
    if (op != 0x0F) {
        if (!priv_one(op)) return 0;
        *code = ST_PRIVILEGED_INSTRUCTION;
        *nparams = 0;
        return 0;
    }
    if (i >= n) return 0;
    UINT32 at = i;                                             /* where the second opcode byte is */
    op = b[i++];
    UINT8 modrm = i < n ? b[i] : 0, reg = (modrm >> 3) & 7;
    if (op == 0xAA) {                                          /* RSM */
        *code = ST_ILLEGAL_INSTRUCTION;
        *nparams = 0;
        return 0;
    }
    if (priv_two(op, reg)) {
        *code = ST_PRIVILEGED_INSTRUCTION;
        *nparams = 0;
        return 0;
    }
    /* A misaligned 16-byte SSE access, in 64-bit code with fixup on */
    if (p->wow || !(t->auto_align || p->auto_align) || i >= n || (modrm >> 6) == 3) return 0;
    UINT64 where;
    UINT8 now;
    if ((op == 0x6F || op == 0x7F) && op66 >= 0 && !rep) {     /* MOVDQA -> MOVDQU */
        where = rip + (UINT64)op66;
        now = 0xF3;
    } else if ((op == 0x28 || op == 0x29) && !rep) {           /* MOVAPS/MOVAPD -> MOVUPS/MOVUPD */
        where = rip + at;
        now = op == 0x28 ? 0x10 : 0x11;
    } else {
        return 0;
    }
    /* through the kernel's mapping: code pages are seldom writable, and
     * an image's page becomes the process's own copy, as on Windows */
    return um_write(p, where, &now, 1) ? 1 : 0;
}
