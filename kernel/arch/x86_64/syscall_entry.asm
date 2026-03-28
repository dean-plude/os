; syscall_entry.asm — KiSystemCall64: SYSCALL fast-path entry
;
; Entered via SYSCALL instruction from user mode.
; On entry (CPU has already done):
;   RCX  ← user RIP (return address for SYSRET)
;   R11  ← user RFLAGS
;   RSP  ← unchanged (still user stack!)
;   CS   ← kernel CS from MSR_STAR[47:32]
;   SS   ← kernel SS from MSR_STAR[47:32]+8
;   IF   ← 0 (cleared by MSR_SFMASK)
;
; NT convention for syscall arguments:
;   RAX = syscall number
;   R10 = arg1 (ntdll moves RCX → R10 before SYSCALL because SYSCALL clobbers RCX)
;   RDX = arg2
;   R8  = arg3
;   R9  = arg4
;
; Our calling convention to KiSystemCallDispatch:
;   RDI = num (RAX)
;   RSI = arg1 (R10)
;   RDX = arg2 (unchanged)
;   RCX = arg3 (R8)
;   R8  = arg4 (R9)
;
; Stack usage: We use the kernel stack (loaded from TSS.RSP0).
; Phase 2: We run entirely in ring 0, so RSP is already the kernel stack.
;          We just need to align the stack and call the C dispatcher.
;
; Note: SWAPGS would normally be used to switch GS to kernel KPCR here,
;       but Phase 2 has no user mode, so we skip SWAPGS.

bits 64

global KiSystemCall64
extern KiSystemCallDispatch

KiSystemCall64:
    ; -----------------------------------------------------------------------
    ; Phase 2: no user mode transitions yet.
    ; RSP is already the kernel stack.  We only need to:
    ;   1. Save the volatile registers that the ABI says we may clobber
    ;      (caller-saved: RAX, RCX, RDX, RSI, RDI, R8-R11)
    ;      — but since we're calling a C function, the C ABI takes care of
    ;      the callee-saved set.  We just need to save RCX/R11 (user RIP/RFLAGS)
    ;      so we can SYSRET later.
    ;   2. Build the argument list for KiSystemCallDispatch.
    ;   3. Call the C dispatcher.
    ;   4. SYSRET.
    ; -----------------------------------------------------------------------

    ; Align stack to 16 bytes (SYSCALL doesn't push a return address,
    ; so RSP is not necessarily aligned at this point).
    ; We push an 8-byte value to make it aligned before the CALL.
    push    rbp
    mov     rbp, rsp

    ; Save user RIP (RCX) and RFLAGS (R11) — needed for SYSRET
    push    rcx         ; user RIP
    push    r11         ; user RFLAGS

    ; Save caller-saved registers we'll clobber
    push    rax         ; syscall number

    ; Align stack: we've pushed 4×8 = 32 bytes after rbp push
    ; RSP needs to be 16-byte aligned before CALL.  Push a dummy if needed.
    ; Stack at this point (relative to saved rbp): 5 pushes = 40 bytes
    ; 40 % 16 = 8 → need one more push to get to 48 → 16-aligned
    sub     rsp, 8      ; alignment pad

    ; Build arguments for KiSystemCallDispatch(num, arg1, arg2, arg3, arg4)
    ; System V AMD64 ABI: RDI, RSI, RDX, RCX, R8
    mov     rdi, rax    ; num = RAX
    mov     rsi, r10    ; arg1 = R10 (ntdll's RCX)
    ; RDX already = arg2
    mov     rcx, r8     ; arg3 = R8
    mov     r8,  r9     ; arg4 = R9

    call    KiSystemCallDispatch

    ; RAX = NTSTATUS return value — pass it back to user in RAX

    ; Restore alignment pad
    add     rsp, 8

    ; Restore saved registers
    pop     rax         ; discard saved syscall number; RAX has return value
    pop     r11         ; user RFLAGS
    pop     rcx         ; user RIP

    ; Save NTSTATUS into a scratch register before we restore RBP
    ; (RAX already has the return value from KiSystemCallDispatch)
    ; We must not clobber RAX now.
    pop     rbp         ; restore caller's RBP (the one we pushed first)

    ; Note: RSP is not restored here — it was never changed because we
    ; stayed on the kernel stack.  For Phase 2 (kernel-only), we're done.

    ; SYSRETQ returns to 64-bit user mode:
    ;   RCX → RIP, R11 → RFLAGS, CS ← user CS, SS ← user SS
    ; (In Phase 2, since we're ring-0-only, this returns to wherever
    ;  the kernel called SYSCALL from — or simply falls through for
    ;  the INT 2E path.)
    sysretq
