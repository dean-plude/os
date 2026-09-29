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

/* the other system DLLs: dllexport while building that DLL */
#if defined(NOVA_BUILD_ADVAPI32)
#define WINADVAPI __declspec(dllexport)
#else
#define WINADVAPI __declspec(dllimport)
#endif
#if defined(NOVA_BUILD_SHELL32)
#define SHSTDAPI_(t) __declspec(dllexport) t __stdcall
#else
#define SHSTDAPI_(t) __declspec(dllimport) t __stdcall
#endif
#if defined(NOVA_BUILD_SHLWAPI)
#define LWSTDAPI_(t) __declspec(dllexport) t __stdcall
#else
#define LWSTDAPI_(t) __declspec(dllimport) t __stdcall
#endif
#if defined(NOVA_BUILD_OLE32)
#define WINOLEAPI_(t) __declspec(dllexport) t __stdcall
#else
#define WINOLEAPI_(t) __declspec(dllimport) t __stdcall
#endif
#if defined(NOVA_BUILD_OLEAUT32)
#define WINOLEAUTAPI_(t) __declspec(dllexport) t __stdcall
#else
#define WINOLEAUTAPI_(t) __declspec(dllimport) t __stdcall
#endif

#ifdef __cplusplus
#define _NOVA_BEGIN extern "C" {
#define _NOVA_END   }
#else
#define _NOVA_BEGIN
#define _NOVA_END
#endif
