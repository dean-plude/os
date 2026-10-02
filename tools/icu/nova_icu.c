/*
 * The NovaOS part of icu.dll: where the ICU data lives.
 *
 * Windows 10 keeps ICU's data next to the system, not inside icu.dll
 * (%SystemRoot%\Globalization\ICU); NovaOS does the same, so the 64-bit and
 * 32-bit icu.dll share one icudt77l.dat.  Unless ICU_DATA names another
 * folder, the data directory is set when the DLL is loaded.
 */
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "unicode/putil.h"

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        const char *env = getenv("ICU_DATA");
        if (!env || !*env) {
            char dir[MAX_PATH + 32];
            UINT n = GetSystemWindowsDirectoryA(dir, MAX_PATH);
            if (n == 0 || n >= MAX_PATH)
                strcpy(dir, "C:\\Windows"), n = 10;
            strcpy(dir + n, "\\Globalization\\ICU");
            u_setDataDirectory(dir);
        }
    }
    return TRUE;
}
