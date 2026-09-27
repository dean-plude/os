/* _nova.h — NovaOS SDK: import/export decoration shared by all headers */
#pragma once

#if defined(NOVA_BUILD_MSVCRT)
#define _CRTIMP __declspec(dllexport)
#else
#define _CRTIMP __declspec(dllimport)
#endif

#if defined(NOVA_BUILD_KERNEL32)
#define WINBASEAPI __declspec(dllexport)
#else
#define WINBASEAPI __declspec(dllimport)
#endif

#if defined(NOVA_BUILD_NTDLL)
#define NTSYSAPI __declspec(dllexport)
#else
#define NTSYSAPI __declspec(dllimport)
#endif

#ifdef __cplusplus
#define _NOVA_BEGIN extern "C" {
#define _NOVA_END   }
#else
#define _NOVA_BEGIN
#define _NOVA_END
#endif
