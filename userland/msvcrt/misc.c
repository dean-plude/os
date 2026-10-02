/* msvcrt: setjmp/longjmp, signal, locale */
#define NOVA_BUILD_MSVCRT
#include <setjmp.h>
#include <signal.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN64
/* jmp_buf: rbx rbp rdi rsi rsp r12 r13 r14 r15 rip, then xmm6-15 at +80 */
__asm__(".globl _setjmp_nova\n"
        ".section .text$setjmp,\"xr\"\n"
        "_setjmp_nova:\n\t"
        "movq %rbx, 0(%rcx)\n\t"
        "movq %rbp, 8(%rcx)\n\t"
        "movq %rdi, 16(%rcx)\n\t"
        "movq %rsi, 24(%rcx)\n\t"
        "leaq 8(%rsp), %rdx\n\t"
        "movq %rdx, 32(%rcx)\n\t"
        "movq %r12, 40(%rcx)\n\t"
        "movq %r13, 48(%rcx)\n\t"
        "movq %r14, 56(%rcx)\n\t"
        "movq %r15, 64(%rcx)\n\t"
        "movq (%rsp), %rdx\n\t"
        "movq %rdx, 72(%rcx)\n\t"
        "movdqu %xmm6, 80(%rcx)\n\t"
        "movdqu %xmm7, 96(%rcx)\n\t"
        "movdqu %xmm8, 112(%rcx)\n\t"
        "movdqu %xmm9, 128(%rcx)\n\t"
        "movdqu %xmm10, 144(%rcx)\n\t"
        "movdqu %xmm11, 160(%rcx)\n\t"
        "movdqu %xmm12, 176(%rcx)\n\t"
        "movdqu %xmm13, 192(%rcx)\n\t"
        "movdqu %xmm14, 208(%rcx)\n\t"
        "movdqu %xmm15, 224(%rcx)\n\t"
        "xorl %eax, %eax\n\t"
        "retq\n\t"
        ".globl longjmp\n"
        "longjmp:\n\t"
        "movl %edx, %eax\n\t"
        "testl %eax, %eax\n\t"
        "jnz 1f\n\t"
        "incl %eax\n"
        "1:\n\t"
        "movq 0(%rcx), %rbx\n\t"
        "movq 8(%rcx), %rbp\n\t"
        "movq 16(%rcx), %rdi\n\t"
        "movq 24(%rcx), %rsi\n\t"
        "movq 40(%rcx), %r12\n\t"
        "movq 48(%rcx), %r13\n\t"
        "movq 56(%rcx), %r14\n\t"
        "movq 64(%rcx), %r15\n\t"
        "movdqu 80(%rcx), %xmm6\n\t"
        "movdqu 96(%rcx), %xmm7\n\t"
        "movdqu 112(%rcx), %xmm8\n\t"
        "movdqu 128(%rcx), %xmm9\n\t"
        "movdqu 144(%rcx), %xmm10\n\t"
        "movdqu 160(%rcx), %xmm11\n\t"
        "movdqu 176(%rcx), %xmm12\n\t"
        "movdqu 192(%rcx), %xmm13\n\t"
        "movdqu 208(%rcx), %xmm14\n\t"
        "movdqu 224(%rcx), %xmm15\n\t"
        "movq 32(%rcx), %rsp\n\t"
        "jmpq *72(%rcx)\n\t"
        ".section .drectve,\"yn\"\n\t"
        ".ascii \" /EXPORT:_setjmp_nova /EXPORT:longjmp\"\n\t"
        ".text\n");

#else
/* jmp_buf (Microsoft's x86 _JUMP_BUFFER): ebp ebx edi esi esp eip, the
 * fs:[0] exception frame, a try level, the "VC20" cookie.  longjmp takes
 * the exception frames between off the chain (their handlers run, as in
 * an unwind) before jumping. */
__asm__(".globl __setjmp_nova\n.globl __setjmp\n.globl __setjmp3\n"
        ".section .text$setjmp,\"xr\"\n"
        "__setjmp_nova:\n__setjmp:\n__setjmp3:\n\t"
        "movl 4(%esp), %edx\n\t"
        "movl %ebp, 0(%edx)\n\t"
        "movl %ebx, 4(%edx)\n\t"
        "movl %edi, 8(%edx)\n\t"
        "movl %esi, 12(%edx)\n\t"
        "leal 4(%esp), %eax\n\t"
        "movl %eax, 16(%edx)\n\t"
        "movl (%esp), %eax\n\t"
        "movl %eax, 20(%edx)\n\t"
        "movl %fs:0, %eax\n\t"
        "movl %eax, 24(%edx)\n\t"
        "movl $-1, 28(%edx)\n\t"
        "movl $0x56433230, 32(%edx)\n\t"
        "movl $0, 36(%edx)\n\t"
        "xorl %eax, %eax\n\t"
        "retl\n\t"
        ".globl _longjmp\n"
        "_longjmp:\n\t"
        "movl 4(%esp), %esi\n\t"
        "movl 8(%esp), %edi\n\t"
        "movl 24(%esi), %eax\n\t"
        "cmpl %fs:0, %eax\n\t"
        "je 2f\n\t"
        "pushl $0\n\t" "pushl $0\n\t" "pushl $0\n\t" "pushl %eax\n\t"
        "calll *__imp__RtlUnwind@16\n\t"
        "movl 24(%esi), %eax\n\t"
        "movl %eax, %fs:0\n"
        "2:\n\t"
        "movl %edi, %eax\n\t"
        "testl %eax, %eax\n\t"
        "jnz 1f\n\t"
        "incl %eax\n"
        "1:\n\t"
        "movl %esi, %edx\n\t"
        "movl 0(%edx), %ebp\n\t"
        "movl 4(%edx), %ebx\n\t"
        "movl 8(%edx), %edi\n\t"
        "movl 12(%edx), %esi\n\t"
        "movl 16(%edx), %esp\n\t"
        "jmpl *20(%edx)\n\t"
        ".section .drectve,\"yn\"\n\t"
        ".ascii \" /EXPORT:__setjmp_nova /EXPORT:__setjmp /EXPORT:__setjmp3 /EXPORT:_longjmp\"\n\t"
        ".text\n");

#endif

static __sighandler_t g_sig[32];

__sighandler_t signal(int sig, __sighandler_t fn)
{
    if (sig <= 0 || sig >= 32) return SIG_ERR;
    __sighandler_t old = g_sig[sig];
    g_sig[sig] = fn;
    return old;
}

int raise(int sig)
{
    if (sig <= 0 || sig >= 32) return -1;
    __sighandler_t h = g_sig[sig];
    if (h == SIG_IGN) return 0;
    if (h && h != SIG_DFL) { g_sig[sig] = SIG_DFL; h(sig); return 0; }
    if (sig == SIGABRT) abort();
    exit(3);
}

struct lconv *localeconv(void)
{
    /* the "C" locale: CHAR_MAX (127) marks the values it leaves unspecified */
    static struct lconv lc = { ".", "", "", "", "", "", "", "", "", "",
                               127, 127, 127, 127, 127, 127, 127, 127,
                               L".", L"", L"", L"", L"", L"", L"", L"" };
    return &lc;
}
