/*
 * NovaOS: Microsoft's <eh.h> (terminate handlers, structured exception
 * translators) for the C++ standard library.
 */
#pragma once
#include <vcruntime.h>

#pragma pack(push, 8)
#ifdef __cplusplus
extern "C" {
#endif
typedef void(__cdecl *terminate_handler)(void);
typedef void(__cdecl *terminate_function)(void);
typedef void(__cdecl *unexpected_handler)(void);
typedef void(__cdecl *unexpected_function)(void);
struct _EXCEPTION_POINTERS;
typedef void(__cdecl *_se_translator_function)(unsigned int, struct _EXCEPTION_POINTERS *);

__declspec(noreturn) void __cdecl abort(void);
__declspec(noreturn) void __cdecl terminate(void) throw();
__declspec(noreturn) void __cdecl unexpected(void);
int __cdecl _is_exception_typeof(const type_info &, struct _EXCEPTION_POINTERS *);
terminate_handler __cdecl set_terminate(terminate_handler) throw();
terminate_handler __cdecl _get_terminate(void);
unexpected_handler __cdecl set_unexpected(unexpected_handler) throw();
unexpected_handler __cdecl _get_unexpected(void);
_se_translator_function __cdecl _set_se_translator(_se_translator_function);
bool __cdecl __uncaught_exception(void);
int __cdecl __uncaught_exceptions(void);
#ifdef __cplusplus
}
#endif
#pragma pack(pop)
