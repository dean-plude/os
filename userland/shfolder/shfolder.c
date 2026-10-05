/*
 * shfolder.dll — SHGetFolderPath for programs written before Windows 2000
 * put it in shell32 (Inno Setup 4 and other installers of that age load
 * shfolder.dll by name to find the Start menu, Program Files and AppData
 * folders).  Windows keeps it as a small DLL that calls shell32's; so does
 * NovaOS.  Its version resource (shfolder.rc) is Windows 10's, so that
 * installers carrying the old redistributable copy (Inno Setup's
 * _shfoldr.dll, which reads the folders from Explorer's registry keys
 * NovaOS does not keep) take this one, as they do on Windows.
 */

#include <windows.h>

__declspec(dllimport) HRESULT WINAPI SHGetFolderPathW(HWND hwnd, int csidl, HANDLE token, DWORD flags, LPWSTR path);
__declspec(dllimport) HRESULT WINAPI SHGetFolderPathA(HWND hwnd, int csidl, HANDLE token, DWORD flags, LPSTR path);

/* exported under shell32's names, which this file imports */
#ifdef _WIN64
#pragma comment(linker, "/export:SHGetFolderPathA=shf_SHGetFolderPathA")
#pragma comment(linker, "/export:SHGetFolderPathW=shf_SHGetFolderPathW")
#else
#pragma comment(linker, "/export:SHGetFolderPathA=_shf_SHGetFolderPathA@20")
#pragma comment(linker, "/export:SHGetFolderPathW=_shf_SHGetFolderPathW@20")
#endif

HRESULT WINAPI shf_SHGetFolderPathA(HWND hwnd, int csidl, HANDLE token, DWORD flags, LPSTR path)
{
    return SHGetFolderPathA(hwnd, csidl, token, flags, path);
}

HRESULT WINAPI shf_SHGetFolderPathW(HWND hwnd, int csidl, HANDLE token, DWORD flags, LPWSTR path)
{
    return SHGetFolderPathW(hwnd, csidl, token, flags, path);
}
