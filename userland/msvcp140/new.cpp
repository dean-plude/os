/*
 * operator new and delete, as the VC runtime's static library gives every
 * MSVC-built module its own: on the C runtime's heap, new throwing
 * bad_alloc once the new handler (_callnewh) gives up.  Linked into the
 * C++ library's DLLs and NovaOS's own C++ programs.  Written for NovaOS.
 */
#include <stdlib.h>
#include <malloc.h>
#include <new>

extern "C" int __cdecl _callnewh(size_t);

void *__cdecl operator new(size_t n)
{
    for (;;) {
        if (void *p = malloc(n ? n : 1)) return p;
        if (_callnewh(n) == 0) {
            static const std::bad_alloc nomem;
            throw nomem;
        }
    }
}
void *__cdecl operator new[](size_t n) { return ::operator new(n); }
void *__cdecl operator new(size_t n, const std::nothrow_t &) noexcept
{
    try { return ::operator new(n); } catch (...) { return nullptr; }
}
void *__cdecl operator new[](size_t n, const std::nothrow_t &) noexcept
{
    try { return ::operator new(n); } catch (...) { return nullptr; }
}
void __cdecl operator delete(void *p) noexcept { free(p); }
void __cdecl operator delete[](void *p) noexcept { free(p); }
void __cdecl operator delete(void *p, size_t) noexcept { free(p); }
void __cdecl operator delete[](void *p, size_t) noexcept { free(p); }
void __cdecl operator delete(void *p, const std::nothrow_t &) noexcept { free(p); }
void __cdecl operator delete[](void *p, const std::nothrow_t &) noexcept { free(p); }

void *__cdecl operator new(size_t n, std::align_val_t al)
{
    for (;;) {
        if (void *p = _aligned_malloc(n ? n : 1, static_cast<size_t>(al))) return p;
        if (_callnewh(n) == 0) throw std::bad_alloc();
    }
}
void *__cdecl operator new[](size_t n, std::align_val_t al) { return ::operator new(n, al); }
void *__cdecl operator new(size_t n, std::align_val_t al, const std::nothrow_t &) noexcept
{
    try { return ::operator new(n, al); } catch (...) { return nullptr; }
}
void *__cdecl operator new[](size_t n, std::align_val_t al, const std::nothrow_t &) noexcept
{
    try { return ::operator new(n, al); } catch (...) { return nullptr; }
}
void __cdecl operator delete(void *p, std::align_val_t) noexcept { _aligned_free(p); }
void __cdecl operator delete[](void *p, std::align_val_t) noexcept { _aligned_free(p); }
void __cdecl operator delete(void *p, size_t, std::align_val_t) noexcept { _aligned_free(p); }
void __cdecl operator delete[](void *p, size_t, std::align_val_t) noexcept { _aligned_free(p); }
void __cdecl operator delete(void *p, std::align_val_t, const std::nothrow_t &) noexcept { _aligned_free(p); }
void __cdecl operator delete[](void *p, std::align_val_t, const std::nothrow_t &) noexcept { _aligned_free(p); }
