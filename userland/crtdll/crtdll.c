/*
 * crtdll.dll — the C runtime of Windows NT 3.x, still in System32 on
 * Windows 10 for the programs built against it (old MinGW builds: the
 * zlib.dll that SuperTux 0.1.3 ships imports fopen and malloc from it).
 * Its functions are msvcrt's, so it forwards to msvcrt.dll (forwards.txt,
 * linked by build.py); only __GetMainArgs, crtdll's name and signature for
 * __getmainargs, is code of its own.
 */

#include <windows.h>

typedef struct { int newmode; } _startupinfo;
__declspec(dllimport) int __cdecl __getmainargs(int *argc, char ***argv, char ***envp, int expand, _startupinfo *si);

__declspec(dllexport) int __cdecl __GetMainArgs(int *argc, char ***argv, char ***envp, int expand)
{
    _startupinfo si = { 0 };
    return __getmainargs(argc, argv, envp, expand, &si);
}
