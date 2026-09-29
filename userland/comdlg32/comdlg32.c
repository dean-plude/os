/*
 * comdlg32.dll — common dialogs.  NovaOS has no dialog windows for
 * programs yet, so the dialogs return as if the user cancelled them
 * (FALSE with CommDlgExtendedError() == 0), which programs handle.
 * GetFileTitle, which needs no window, works.
 */

#include <windows.h>

#define CDAPI __declspec(dllexport)

CDAPI DWORD WINAPI CommDlgExtendedError(void) { return 0; }
CDAPI BOOL WINAPI GetOpenFileNameW(void *ofn) { (void)ofn; return FALSE; }
CDAPI BOOL WINAPI GetOpenFileNameA(void *ofn) { (void)ofn; return FALSE; }
CDAPI BOOL WINAPI GetSaveFileNameW(void *ofn) { (void)ofn; return FALSE; }
CDAPI BOOL WINAPI GetSaveFileNameA(void *ofn) { (void)ofn; return FALSE; }
CDAPI BOOL WINAPI ChooseColorW(void *cc) { (void)cc; return FALSE; }
CDAPI BOOL WINAPI ChooseColorA(void *cc) { (void)cc; return FALSE; }
CDAPI BOOL WINAPI ChooseFontW(void *cf) { (void)cf; return FALSE; }
CDAPI BOOL WINAPI ChooseFontA(void *cf) { (void)cf; return FALSE; }
CDAPI BOOL WINAPI PrintDlgW(void *pd) { (void)pd; return FALSE; }
CDAPI BOOL WINAPI PrintDlgA(void *pd) { (void)pd; return FALSE; }
CDAPI HRESULT WINAPI PrintDlgExW(void *pd) { (void)pd; return (HRESULT)0x80004005L; }
CDAPI BOOL WINAPI PageSetupDlgW(void *psd) { (void)psd; return FALSE; }
CDAPI HWND WINAPI FindTextW(void *fr) { (void)fr; return 0; }
CDAPI HWND WINAPI ReplaceTextW(void *fr) { (void)fr; return 0; }

/* The file's name without its directory */
CDAPI short WINAPI GetFileTitleW(LPCWSTR file, LPWSTR buf, WORD n)
{
    const WCHAR *name = file;
    for (const WCHAR *c = file; *c; c++) if (*c == '\\' || *c == '/' || *c == ':') name = c + 1;
    if (!*name) return -1;
    int len = 0;
    while (name[len]) len++;
    if (!buf || n <= len) return (short)(len + 1);
    for (int i = 0; i <= len; i++) buf[i] = name[i];
    return 0;
}

CDAPI short WINAPI GetFileTitleA(LPCSTR file, LPSTR buf, WORD n)
{
    const char *name = file;
    for (const char *c = file; *c; c++) if (*c == '\\' || *c == '/' || *c == ':') name = c + 1;
    if (!*name) return -1;
    int len = 0;
    while (name[len]) len++;
    if (!buf || n <= len) return (short)(len + 1);
    for (int i = 0; i <= len; i++) buf[i] = name[i];
    return 0;
}
