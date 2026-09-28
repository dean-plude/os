/* tlssup.c — implicit TLS support (what the MSVC CRT's tlssup.c provides):
 * the .tls section markers, the TLS callback array, _tls_index and the
 * IMAGE_TLS_DIRECTORY (_tls_used) the linker turns into the TLS data
 * directory.  Linked into every module that may use __declspec(thread). */
#include <windows.h>
#include <winnt.h>

unsigned long _tls_index = 0;

#pragma section(".tls", long, read, write)
#pragma section(".tls$ZZZ", long, read, write)
__declspec(allocate(".tls")) char _tls_start = 0;
__declspec(allocate(".tls$ZZZ")) char _tls_end = 0;

#pragma section(".CRT$XLA", long, read)
#pragma section(".CRT$XLZ", long, read)
__declspec(allocate(".CRT$XLA")) PIMAGE_TLS_CALLBACK __xl_a = 0;
__declspec(allocate(".CRT$XLZ")) PIMAGE_TLS_CALLBACK __xl_z = 0;

#pragma section(".rdata$T", long, read)
__declspec(allocate(".rdata$T")) const IMAGE_TLS_DIRECTORY64 _tls_used = {
    (ULONGLONG)(ULONG_PTR)&_tls_start,
    (ULONGLONG)(ULONG_PTR)&_tls_end,
    (ULONGLONG)(ULONG_PTR)&_tls_index,
    (ULONGLONG)(ULONG_PTR)(&__xl_a + 1),
    0, 0
};
