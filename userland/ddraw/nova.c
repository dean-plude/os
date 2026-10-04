/*
 * ddraw.dll: what cnc-ddraw (third_party/cnc-ddraw) takes from the
 * libraries MinGW-w64 links it with (libuuid, the C runtime's startup
 * objects, avifil32's import library), for NovaOS's link.
 */
#include <windows.h>

const GUID GUID_NULL = { 0 };

int _fltused = 0x9875;          /* floating point in use (the compiler references it) */

/* cnc-ddraw's hook list names avifil32's AVIStreamGetFrameOpen (for games
 * whose videos it scales; NovaOS's ddraw.ini turns hooking off).  NovaOS has
 * no avifil32.dll, so it is looked up when called, not imported. */
void *WINAPI AVIStreamGetFrameOpen(void *stream, void *wanted)
{
    HMODULE m = LoadLibraryA("avifil32.dll");
    void *(WINAPI *fn)(void *, void *) = m ? (void *(WINAPI *)(void *, void *))GetProcAddress(m, "AVIStreamGetFrameOpen") : NULL;
    return fn ? fn(stream, wanted) : NULL;
}
