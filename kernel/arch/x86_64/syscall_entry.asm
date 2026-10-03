; syscall_entry.asm — KiSystemCall64: SYSCALL fast-path entry
;
; Entered via SYSCALL instruction from user mode (CPL=3 → CPL=0).
; On entry (CPU has already done):
;   RCX  ← user RIP (return address for SYSRET)
;   R11  ← user RFLAGS
;   RSP  ← unchanged (still user stack!)
;   CS   ← kernel CS from MSR_STAR[47:32]
;   SS   ← kernel SS from MSR_STAR[47:32]+8
;   IF   ← 0 (cleared by MSR_SFMASK)
;
; NT convention for syscall arguments (ntdll syscall stub):
;   RAX = syscall number
;   R10 = arg1 (ntdll moves RCX → R10 before SYSCALL because SYSCALL clobbers RCX)
;   RDX = arg2
;   R8  = arg3
;   R9  = arg4
;
; Our calling convention to KiSystemCallDispatch (System V AMD64):
;   RDI = num (RAX)
;   RSI = arg1 (R10)
;   RDX = arg2 (unchanged)
;   RCX = arg3 (R8)
;   R8  = arg4 (R9)
;
; Phase 5 additions:
;   SWAPGS on entry  — GS now points to KPCR, user GS saved in MSR_GS_BASE
;   Save user RSP in KPCR.UserRsp
;   Load kernel stack from KPCR.KernelRsp (or TSS.RSP0 for nested calls)
;   SWAPGS on exit   — restores user GS (TEB pointer)
;
; KPCR offsets (must match kpcr.h):
%define KPCR_CURRENT_THREAD  0x08
%define KPCR_USER_RSP        0x20
%define KPCR_KERNEL_RSP      0x28

bits 64

global KiSystemCall64
extern KiSystemCallEntry
extern gs_mismatch

KiSystemCall64:
    ; -----------------------------------------------------------------------
    ; Phase 5: user mode is live — SWAPGS to switch GS to KPCR.
    ; -----------------------------------------------------------------------
    swapgs                  ; GS → KPCR, user GS saved in MSR_KERNEL_GS_BASE

    ; Save user RSP and switch to kernel stack.
    ; KPCR.UserRsp = user RSP so we can restore it on SYSRET.
    mov     gs:[KPCR_USER_RSP], rsp

    ; Load kernel stack.  KPCR.KernelRsp is set by the scheduler to the
    ; top of the current thread's kernel stack.
    mov     rsp, gs:[KPCR_KERNEL_RSP]

    ; -----------------------------------------------------------------------
    ; We are now on the kernel stack.
    ; Push a minimal trap frame:
    ;   [rsp+0]  = user RFLAGS (R11)
    ;   [rsp+8]  = user RIP    (RCX)
    ;   [rsp+16] = user RSP    (saved from KPCR)
    ;   [rsp+24] = rbp         (saved for frame pointer chain)
    ; -----------------------------------------------------------------------
    push    r11                         ; user RFLAGS
    push    rcx                         ; user RIP
    push    qword gs:[KPCR_USER_RSP]    ; user RSP
    push    rbp
    mov     rbp, rsp
    ; RDI and RSI are callee-saved in the Windows x64 ABI but not in the
    ; System V ABI our C code uses, so preserve them for the caller.
    push    rdi
    push    rsi

    ; 6 qwords pushed onto a 16-byte-aligned KernelRsp: aligned for the call.

    ; -----------------------------------------------------------------------
    ; KiSystemCallDispatch(num, arg1, arg2, arg3, arg4, user_rsp)
    ; Arguments 5+ are on the user stack at user_rsp + 0x28 (after the
    ; return address and the 32-byte home area), as in the Windows ABI.
    ; -----------------------------------------------------------------------
    mov     rdi, rax        ; num  = RAX (syscall number)
    mov     rsi, r10        ; arg1 = R10 (ntdll moved RCX → R10)
    ; RDX = arg2 (unchanged)
    mov     rcx, r8         ; arg3 = R8
    mov     r8,  r9         ; arg4 = R9
    mov     r9,  [rbp + 8]  ; user RSP

    call    KiSystemCallEntry      ; takes the kernel lock around the dispatch
    ; RAX = return value (interrupts are disabled again here)

    ; GS must still be the KPCR: the SWAPGS below gives the program its own
    mov     r10, rax
    mov     ecx, 0xC0000101         ; MSR_GS_BASE
    rdmsr
    test    edx, edx                ; a kernel address: bit 63 set
    js      .gs_ok
    xor     edi, edi
    mov     esi, 2
    call    gs_mismatch             ; does not return
.gs_ok:
    mov     rax, r10

    ; -----------------------------------------------------------------------
    ; Return path
    ; -----------------------------------------------------------------------
    pop     rsi
    pop     rdi
    pop     rbp
    pop     qword gs:[KPCR_USER_RSP]  ; user RSP → KPCR (restored below)
    pop     rcx             ; user RIP → RCX (SYSRET uses this)
    pop     r11             ; user RFLAGS → R11 (SYSRET uses this)

    ; Restore user RSP
    mov     rsp, gs:[KPCR_USER_RSP]

    ; SWAPGS restores user GS (TEB pointer)
    swapgs

    ; SYSRETQ: RCX→RIP, R11→RFLAGS, switches to CPL=3.
    ; NASM spells the 64-bit form "o64 sysret" — a bare "sysretq" is
    ; silently assembled as a label, leaving no return instruction at all.
    o64 sysret

; ---------------------------------------------------------------------------
; KiSystemCall32 — SYSCALL from compatibility mode (MSR_CSTAR)
;
; 32-bit programs enter the kernel through int 0x2E; a SYSCALL in 32-bit
; code (AMD CPUs execute it, Intel ones raise #UD) is a program error.
; Move to this thread's kernel stack like KiSystemCall64 and end the
; program there (KiCompatSyscall does not return).
; ---------------------------------------------------------------------------
global KiSystemCall32
extern KiCompatSyscall

KiSystemCall32:
    swapgs
    mov     gs:[KPCR_USER_RSP], rsp
    mov     rsp, gs:[KPCR_KERNEL_RSP]
    and     rsp, -16
    mov     rdi, rcx                    ; the 32-bit caller's return address
    call    KiCompatSyscall
.hang:
    hlt
    jmp     .hang
