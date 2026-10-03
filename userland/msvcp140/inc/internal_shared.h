/*
 * NovaOS: the few helpers of the VC runtime's private <internal_shared.h>
 * that the STL's sources use: the CRT heap (here, the C runtime's own
 * malloc), stack-or-heap buffers and owning pointers, interlocked pointer
 * access.  Written for NovaOS.
 */
#pragma once
#include <crtdefs.h>
#include <malloc.h>
#include <stdlib.h>
#include <intrin.h>
#include <windows.h>
#ifdef __cplusplus
#include <new>
#endif

#define _malloc_crt malloc
#define _calloc_crt calloc
#define _realloc_crt realloc
#define _free_crt free

#ifndef _ASSERTE
#define _ASSERTE(expr) ((void)0)
#endif
#ifndef _ASSERT
#define _ASSERT(expr) ((void)0)
#endif

#ifdef __cplusplus
/* _malloca's buffers: here always on the heap, freed by the scoped pointer */
#define _malloca_crt_t(t, n) (static_cast<t *>(malloc((n) * sizeof(t))))

template <typename T>
class __crt_scoped_stack_ptr {
public:
    explicit __crt_scoped_stack_ptr(T *p) noexcept : _p(p) {}
    ~__crt_scoped_stack_ptr() noexcept { free(_p); }
    __crt_scoped_stack_ptr(const __crt_scoped_stack_ptr &) = delete;
    __crt_scoped_stack_ptr &operator=(const __crt_scoped_stack_ptr &) = delete;
    T *get() const noexcept { return _p; }
    explicit operator bool() const noexcept { return _p != nullptr; }
private:
    T *_p;
};

template <typename T>
class __crt_unique_heap_ptr {
public:
    __crt_unique_heap_ptr() noexcept : _p(nullptr) {}
    explicit __crt_unique_heap_ptr(T *p) noexcept : _p(p) {}
    __crt_unique_heap_ptr(__crt_unique_heap_ptr &&o) noexcept : _p(o._p) { o._p = nullptr; }
    ~__crt_unique_heap_ptr() noexcept { free(_p); }
    __crt_unique_heap_ptr &operator=(__crt_unique_heap_ptr &&o) noexcept {
        if (this != &o) { free(_p); _p = o._p; o._p = nullptr; }
        return *this;
    }
    __crt_unique_heap_ptr &operator=(T *p) noexcept { free(_p); _p = p; return *this; }
    __crt_unique_heap_ptr(const __crt_unique_heap_ptr &) = delete;
    __crt_unique_heap_ptr &operator=(const __crt_unique_heap_ptr &) = delete;
    T *get() const noexcept { return _p; }
    T *detach() noexcept { T *p = _p; _p = nullptr; return p; }
    explicit operator bool() const noexcept { return _p != nullptr; }
    T &operator[](size_t i) const noexcept { return _p[i]; }
private:
    T *_p;
};

/* _malloc_crt_t and _calloc_crt_t give owning pointers */
#define _malloc_crt_t(t, n) (__crt_unique_heap_ptr<t>(static_cast<t *>(malloc((n) * sizeof(t)))))
#define _calloc_crt_t(t, n) (__crt_unique_heap_ptr<t>(static_cast<t *>(calloc((n), sizeof(t)))))

/* initializer sections (the DLL's startup runs .CRT$XI* then .CRT$XC*) */
typedef int(__cdecl *_PIFV)(void);
typedef void(__cdecl *_PVFV)(void);
#pragma section(".CRT$XIC", long, read)
#pragma section(".CRT$XCC", long, read)
#define _CRTALLOC(x) __declspec(allocate(x))

template <typename T>
inline T *__crt_interlocked_read_pointer(T *const volatile *p) noexcept {
    return static_cast<T *>(_InterlockedCompareExchangePointer(
        reinterpret_cast<void *volatile *>(const_cast<T **>(p)), nullptr, nullptr));
}
template <typename T, typename U>
inline T *__crt_interlocked_exchange_pointer(T *volatile *p, U v) noexcept {
    return static_cast<T *>(_InterlockedExchangePointer(reinterpret_cast<void *volatile *>(p), static_cast<T *>(v)));
}
#endif
