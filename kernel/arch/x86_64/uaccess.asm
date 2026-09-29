; uaccess.asm — copying to and from user memory, surviving page faults
;
; Another thread of the program may unmap the memory while the kernel
; copies it (on another CPU).  Every instruction that touches user memory
; is between uaccess_begin and uaccess_end; a page fault there is sent to
; uaccess_fault (UserCopyFixup in probe.c), which makes the routine return
; 1 instead of 0.  The routines use no stack, so the fault path can simply
; return to the caller.

[BITS 64]
[DEFAULT REL]

SECTION .text

GLOBAL uaccess_begin
GLOBAL uaccess_end
GLOBAL uaccess_fault
GLOBAL uaccess_copy
GLOBAL uaccess_zero

; int uaccess_copy(void *dst, const void *src, size_t len)   (memmove)
uaccess_copy:
    mov     rcx, rdx
    cmp     rdi, rsi
    jbe     uaccess_copy_fwd
    lea     rax, [rsi + rdx]
    cmp     rdi, rax
    jae     uaccess_copy_fwd
    ; overlapping, destination above: copy backwards
    lea     rsi, [rsi + rdx - 1]
    lea     rdi, [rdi + rdx - 1]
    std
uaccess_begin:
    rep movsb
    cld
    xor     eax, eax
    ret
uaccess_copy_fwd:
    rep movsb
    xor     eax, eax
    ret

; int uaccess_zero(void *dst, size_t len)
uaccess_zero:
    mov     rcx, rsi
    xor     eax, eax
    rep stosb
uaccess_end:
    ret

uaccess_fault:
    cld
    mov     eax, 1
    ret
