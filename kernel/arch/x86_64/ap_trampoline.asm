; ap_trampoline.asm — start-up code for the other CPUs (application processors)
;
; smp.c copies the bytes between ap_trampoline_start and ap_trampoline_end
; to physical 0x8000 (TRAMP_BASE: the addresses below are those of the
; copy), fills in the data block at the end, and sends the CPU a STARTUP
; IPI naming that page.  The CPU starts in
; 16-bit real mode at the first byte and goes:
;
;   real mode → 32-bit protected mode → long mode (paging with ap_cr3,
;   whose low 2 MiB are identity-mapped) → 64-bit kernel code at ap_entry
;
; with RSP = ap_stack and RDI = ap_arg.  NXE is set before paging goes on
; because the kernel's page tables use the NX bit.

[BITS 16]

SECTION .rodata

GLOBAL ap_trampoline_start
GLOBAL ap_trampoline_end
GLOBAL ap_trampoline_data

; Address of a label in the copy
%define TRAMP_BASE 0x8000
%define A(x) ((x) - ap_trampoline_start + TRAMP_BASE)

ap_trampoline_start:
    cli
    cld
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    lgdt    [A(tramp_gdtr)]
    mov     eax, cr0
    or      eax, 1                      ; PE
    mov     cr0, eax
    jmp     dword 0x08:A(pm32)

[BITS 32]
pm32:
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     eax, cr4
    or      eax, (1 << 5)               ; PAE
    mov     cr4, eax
    mov     eax, [A(ap_cr3)]
    mov     cr3, eax
    mov     ecx, 0xC0000080             ; EFER
    rdmsr
    or      eax, (1 << 8) | (1 << 11)   ; LME | NXE
    wrmsr
    mov     eax, cr0
    or      eax, 0x80000001             ; PG | PE
    mov     cr0, eax
    jmp     0x18:A(lm64)

[BITS 64]
lm64:
    mov     ax, 0x20
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     rsp, [A(ap_stack)]
    mov     rdi, [A(ap_arg)]
    mov     rax, [A(ap_entry)]
    xor     ebp, ebp
    call    rax                         ; never returns
.hang:
    cli
    hlt
    jmp     .hang

align 16
tramp_gdt:
    dq      0
    dq      0x00CF9A000000FFFF          ; 0x08: 32-bit code
    dq      0x00CF92000000FFFF          ; 0x10: 32-bit data
    dq      0x00AF9A000000FFFF          ; 0x18: 64-bit code
    dq      0x00CF92000000FFFF          ; 0x20: data
tramp_gdtr:
    dw      tramp_gdtr - tramp_gdt - 1
    dd      A(tramp_gdt)

align 8
ap_trampoline_data:                     ; filled in by smp.c (keep in sync)
ap_cr3:     dq 0                        ; +0  page table (below 4 GiB)
ap_entry:   dq 0                        ; +8  64-bit entry point
ap_stack:   dq 0                        ; +16 stack top
ap_arg:     dq 0                        ; +24 first argument (CPU number)
ap_trampoline_end:
