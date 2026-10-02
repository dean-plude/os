/* vterm_libc.h — the C library libvterm (third_party/libvterm) uses, in the
 * kernel: the heap, formatting through ksnprintf, its debug output to the
 * kernel log, and abort() as a panic (libvterm aborts only on broken
 * invariants).  Included ahead of each libvterm source (CMakeLists.txt). */
#pragma once
#include <stddef.h>
#include <stdarg.h>
#include "../mm/vmm.h"
#include "../ke/printf.h"
#include "../lib/string.h"

static inline void *malloc(size_t n) { return kmalloc(n); }
static inline void  free(void *p)    { kfree(p); }
static inline int   abs(int v)       { return v < 0 ? -v : v; }
#define abort()   KPANIC("libvterm: abort()")
#define exit(n)   KPANIC("libvterm: exit(%d)", (int)(n))
#define stderr    ((void *)0)
#define snprintf  ksnprintf
#define vsnprintf kvsnprintf
#define printf    kprintf
#define fprintf(f, ...) kprintf(__VA_ARGS__)
