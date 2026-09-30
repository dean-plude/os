#pragma once
#include <_nova.h>
_NOVA_BEGIN
#ifdef __x86_64__
/* rbx rbp rdi rsi rsp r12-r15 rip (+ xmm6-15 as 16-byte slots) */
typedef __attribute__((aligned(16))) unsigned long long jmp_buf[32];
#else
/* Microsoft's x86 layout: ebp ebx edi esi esp eip, the fs:[0] frame, ... */
typedef int jmp_buf[16];
#endif
_CRTIMP __attribute__((returns_twice)) int _setjmp_nova(jmp_buf env);
_CRTIMP __declspec(noreturn) void longjmp(jmp_buf env, int val);
#define setjmp(env) _setjmp_nova(env)
_NOVA_END
