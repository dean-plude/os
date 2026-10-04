/* libxml2's config.h for NovaOS (msxml6.dll): Windows, no sockets, no
 * zlib, no dlopen; threads through Win32 (libxml2's threads.c) */
#define HAVE_STDINT_H 1
#define VERSION "2.13.8"
/* NovaOS's winnt.h has it as a function; threads.c tests for a macro */
#define InterlockedCompareExchangePointer InterlockedCompareExchangePointer
/* xmlIO.c's Windows file code (msxml6 reads files itself, but it links) */
#include <fcntl.h>
#define MB_ERR_INVALID_CHARS 0x00000008
#define _O_RDONLY O_RDONLY
#define _O_WRONLY O_WRONLY
#define _O_CREAT  O_CREAT
#define _O_TRUNC  O_TRUNC
#ifndef _O_BINARY
#define _O_BINARY 0x8000
#endif
typedef long NTSTATUS;
#include <wchar.h>
#include <unistd.h>
__declspec(dllimport) int _wopen(const wchar_t *path, int flags, ...);
