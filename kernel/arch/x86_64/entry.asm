; entry.asm — x86_64 kernel entry point
;
; This is the VERY FIRST code that executes after the UEFI bootloader
; jumps to us.  At this point:
;
;   - We are in 64-bit long mode with paging enabled
;   - The bootloader's page tables are active (identity map + physmap + kernel)
;   - RDI = physical address of BootInfo struct
;   - We are executing at our virtual link address (0xFFFFFFFF80xxxxxx)
;   - The stack pointer points somewhere in the physmap (bootloader's stack)
;   - All segment registers have valid 64-bit descriptors from UEFI
;
; Our jobs:
;   1. Set up a proper early kernel stack (we'll use a statically allocated
;      .bss stack until the scheduler provides per-thread stacks)
;   2. Zero the .bss section
;   3. Call KiSystemStartup(BootInfo*) — the C entry point
;   4. Halt if KiSystemStartup returns (it shouldn't)

[BITS 64]
[DEFAULT REL]

SECTION .text.entry  ; Placed first by the linker script

EXTERN KiSystemStartup
EXTERN __bss_start
EXTERN __bss_end
EXTERN __kernel_stack_top

GLOBAL _start
_start:
    ; Save the BootInfo pointer (RDI) before we clobber registers
    mov     r15, rdi

    ; Switch to our static kernel stack
    ; (__kernel_stack_top is defined at the end of .bss)
    lea     rsp, [__kernel_stack_top]
    xor     rbp, rbp            ; Clear frame pointer (clean backtraces)

    ; Zero the BSS section
    ; Using REP STOSD for speed — memset isn't available yet
    lea     rdi, [__bss_start]
    lea     rcx, [__bss_end]
    sub     rcx, rdi            ; byte count
    xor     eax, eax            ; fill value = 0
    test    rcx, rcx
    jz      .bss_done
    ; Zero word by word (8 bytes at a time)
    shr     rcx, 3              ; count in qwords
    rep stosq
    ; Handle trailing bytes (BSS should be page-aligned, but be safe)
    lea     rdi, [__bss_start]
    lea     rcx, [__bss_end]
    sub     rcx, rdi
    and     rcx, 7
    rep stosb
.bss_done:

    ; Restore BootInfo pointer
    mov     rdi, r15

    ; Align stack to 16 bytes (System V ABI requirement before function calls)
    and     rsp, -16

    ; Call the C kernel entry point
    ; KiSystemStartup(const BootInfo *info_phys)
    call    KiSystemStartup

    ; Should never return — halt the CPU
.halt:
    cli
    hlt
    jmp     .halt
