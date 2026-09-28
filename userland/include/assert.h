#pragma once
#include <_nova.h>
_NOVA_BEGIN
_CRTIMP void _assert(const char *expr, const char *file, unsigned line);
_NOVA_END
#undef assert
#ifdef NDEBUG
#define assert(e) ((void)0)
#else
#define assert(e) ((e) ? (void)0 : _assert(#e, __FILE__, __LINE__))
#endif
