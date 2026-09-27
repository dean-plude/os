; isr_stubs.asm — x86_64 interrupt/exception entry stubs
;
; Each exception/interrupt needs a small assembly stub that:
;   1. Pushes a dummy error code (0) for exceptions that don't have one
;   2. Pushes the vector number
;   3. Saves all GPRs in the order expected by InterruptFrame
;   4. Calls the C dispatcher (interrupt_dispatch)
;   5. Restores GPRs and returns via IRETQ
;
; We generate the 256 stubs with macros to avoid repetition.
; The linker exports them in an array so idt_init() can install them.
;
; Conventions:
;   - System V AMD64 ABI for the call to interrupt_dispatch
;   - RSP is 16-byte aligned before the call (per ABI)
;
; Calling convention note:
;   When the CPU delivers an exception in ring 0, the stack already
;   has (from bottom): SS, RSP, RFLAGS, CS, RIP (and maybe error code).
;   We push our registers on top.

[BITS 64]
[DEFAULT REL]

SECTION .text

; External C dispatcher
EXTERN interrupt_dispatch

; -----------------------------------------------------------------------
; Macros to generate stubs
; -----------------------------------------------------------------------

; ISR stub for exceptions WITHOUT an error code (CPU doesn't push one)
; We push 0 as a dummy so the frame layout is uniform.
%macro ISR_NOERR 1
isr_stub_%1:
    push    qword 0         ; dummy error code
    push    qword %1        ; vector number
    jmp     isr_common
%endmacro

; ISR stub for exceptions WITH an error code (CPU pushes it automatically)
%macro ISR_ERR 1
isr_stub_%1:
    push    qword %1        ; vector number (error code already on stack)
    jmp     isr_common
%endmacro

; -----------------------------------------------------------------------
; Exception stubs (vectors 0–31)
; -----------------------------------------------------------------------
ISR_NOERR   0    ; #DE Divide Error
ISR_NOERR   1    ; #DB Debug
ISR_NOERR   2    ; NMI
ISR_NOERR   3    ; #BP Breakpoint
ISR_NOERR   4    ; #OF Overflow
ISR_NOERR   5    ; #BR Bound Range Exceeded
ISR_NOERR   6    ; #UD Invalid Opcode
ISR_NOERR   7    ; #NM Device Not Available
ISR_ERR     8    ; #DF Double Fault        (error code = 0 always, but CPU pushes it)
ISR_NOERR   9    ; Coprocessor Segment Overrun (legacy)
ISR_ERR     10   ; #TS Invalid TSS
ISR_ERR     11   ; #NP Segment Not Present
ISR_ERR     12   ; #SS Stack-Segment Fault
ISR_ERR     13   ; #GP General Protection
ISR_ERR     14   ; #PF Page Fault
ISR_NOERR   15   ; Reserved
ISR_NOERR   16   ; #MF x87 FP Exception
ISR_ERR     17   ; #AC Alignment Check
ISR_NOERR   18   ; #MC Machine Check
ISR_NOERR   19   ; #XF SIMD FP Exception
ISR_NOERR   20   ; #VE Virtualization Exception
ISR_ERR     21   ; #CP Control Protection
ISR_NOERR   22
ISR_NOERR   23
ISR_NOERR   24
ISR_NOERR   25
ISR_NOERR   26
ISR_NOERR   27
ISR_NOERR   28
ISR_NOERR   29
ISR_NOERR   30
ISR_NOERR   31

; -----------------------------------------------------------------------
; IRQ stubs (vectors 32–63, mapped from APIC)
; -----------------------------------------------------------------------
%assign i 32
%rep 32
ISR_NOERR i
%assign i i+1
%endrep

; -----------------------------------------------------------------------
; Syscall vector 0x2E (46 decimal — same as 0x2E = 46)
; Generate stubs 64–0xFF, including 0x2E
; -----------------------------------------------------------------------
%assign i 64
%rep 192
ISR_NOERR i
%assign i i+1
%endrep

; -----------------------------------------------------------------------
; Common ISR handler
;
; Stack layout when we arrive here (after the stub's pushes):
;   [rsp+0]   vector
;   [rsp+8]   error_code  (or 0)
;   [rsp+16]  rip
;   [rsp+24]  cs
;   [rsp+32]  rflags
;   [rsp+40]  rsp_user    (only if ring change)
;   [rsp+48]  ss_user     (only if ring change)
;
; We need to push all GPRs to build a complete InterruptFrame.
; Order: rax, rcx, rdx, rbx, rbp, rsi, rdi, r8–r15
; (reversed from InterruptFrame since we push low-to-high)
; -----------------------------------------------------------------------
isr_common:
    ; Save all GPRs (in order that builds InterruptFrame in reverse)
    push    rax
    push    rcx
    push    rdx
    push    rbx
    push    rbp
    push    rsi
    push    rdi
    push    r8
    push    r9
    push    r10
    push    r11
    push    r12
    push    r13
    push    r14
    push    r15

    ; RSP now points to the bottom of InterruptFrame (r15 field).
    ; Pass pointer to frame as first argument (RDI per System V ABI).
    mov     rdi, rsp

    ; Stack alignment: in 64-bit mode the CPU aligns RSP to 16 bytes before
    ; pushing its 5-qword frame (SS, RSP, RFLAGS, CS, RIP).  On top of that
    ; we have vector + error_code (2) and 15 GPRs: 5 + 2 + 15 = 22 qwords =
    ; 176 bytes ≡ 0 (mod 16), so RSP is already aligned for the call.

    ; Call C handler
    call    interrupt_dispatch

    ; Restore GPRs
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     rdi
    pop     rsi
    pop     rbp
    pop     rbx
    pop     rdx
    pop     rcx
    pop     rax

    ; Remove vector and error code
    add     rsp, 16

    ; Return from interrupt (restores RIP, CS, RFLAGS, and RSP/SS if ring change)
    iretq

; -----------------------------------------------------------------------
; Export the stub address table
; -----------------------------------------------------------------------
SECTION .data

GLOBAL isr_stub_table
isr_stub_table:
%assign i 0
%rep 256
    dq  isr_stub_%+i
%assign i i+1
%endrep
