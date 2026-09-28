/* regsvr32.exe [/u] [/s] DLL — calls a COM server's DllRegisterServer (or DllUnregisterServer) */
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef HRESULT (__stdcall *REGFN)(void);

int main(int argc, char **argv)
{
    int unreg = 0, silent = 0;
    const char *dll = 0;
    for (int i = 1; i < argc; i++) {
        if (!_stricmp(argv[i], "/u") || !_stricmp(argv[i], "-u")) unreg = 1;
        else if (!_stricmp(argv[i], "/s") || !_stricmp(argv[i], "-s")) silent = 1;
        else dll = argv[i];
    }
    if (!dll) { fprintf(stderr, "usage: regsvr32 [/u] [/s] DLL\n"); return 1; }
    const char *fn = unreg ? "DllUnregisterServer" : "DllRegisterServer";
    HMODULE m = LoadLibraryA(dll);
    if (!m) { fprintf(stderr, "The module \"%s\" failed to load (error %lu).\n", dll, GetLastError()); return 3; }
    REGFN f = (REGFN)GetProcAddress(m, fn);
    if (!f) { fprintf(stderr, "The module \"%s\" has no entry point %s.\n", dll, fn); return 4; }
    HRESULT hr = f();
    if (FAILED(hr)) { fprintf(stderr, "%s in \"%s\" failed: 0x%08lX\n", fn, dll, (unsigned long)hr); return 5; }
    if (!silent) printf("%s in %s succeeded.\n", fn, dll);
    return 0;
}
