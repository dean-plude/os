/*
 * NovaOS: the VC runtime's private <trnsctrl.h> as excptptr.cpp uses it:
 * the exception being handled on this thread (vcruntime140's
 * __current_exception) and calls of copy constructors and destructors.
 */
#pragma once
#include <ehdata.h>

extern "C" __declspec(dllimport) void **__cdecl __current_exception(void);
#define _pCurrentException (*reinterpret_cast<EHExceptionRecord **>(__current_exception()))

inline void _CallMemberFunction0(void *pthis, void *pmfn) {
    reinterpret_cast<void(__thiscall *)(void *)>(pmfn)(pthis);
}
inline void _CallMemberFunction1(void *pthis, void *pmfn, void *pthat) {
    reinterpret_cast<void(__thiscall *)(void *, void *)>(pmfn)(pthis, pthat);
}
inline void _CallMemberFunction2(void *pthis, void *pmfn, void *pthat, int val2) {
    reinterpret_cast<void(__thiscall *)(void *, void *, int)>(pmfn)(pthis, pthat, val2);
}
