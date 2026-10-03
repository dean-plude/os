/*
 * comdlg32.dll — common dialogs.  The file dialogs are in filedlg.c
 * (GetOpenFileName, GetSaveFileName) and ifiledlg.c (IFileOpenDialog,
 * IFileSaveDialog).  The colour, font, print and find dialogs are not
 * there yet: they return as if the user cancelled them (FALSE with
 * CommDlgExtendedError() == 0), which programs handle.
 */

#include <windows.h>

#define CDAPI __declspec(dllexport)

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
