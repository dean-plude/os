/*
 * display.c — display modes: EnumDisplaySettings, ChangeDisplaySettings
 *
 * The modes are the kernel display driver's (NtNovaGuiCtl CTL_DISPLAY_MODE
 * and CTL_SET_DISPLAY): the Bochs/QEMU VBE adapter can switch between
 * them at run time, other adapters only report the boot mode.
 */
#include "u32.h"

#define DISPLAY_NAME "NovaOS Display"

/* Display modes come from the kernel's display driver (NtNovaGuiCtl
 * CTL_DISPLAY_MODE): mode i = 0, 1, ... largest first, or
 * ENUM_CURRENT_SETTINGS (-1) / ENUM_REGISTRY_SETTINGS (-2); always 32 bpp */
static BOOL display_mode(DWORD mode, DWORD m[4])
{
    return NtNovaGuiCtl(0, CTL_DISPLAY_MODE, (ULONG_PTR)(LONG_PTR)(LONG)mode, m) != 0;
}
USERAPI BOOL EnumDisplaySettingsW(LPCWSTR dev, DWORD mode, void *dm)
{
    (void)dev;
    DWORD m[4];
    if (!dm || !display_mode(mode, m)) return FALSE;
    BYTE *b = dm;                                           /* DEVMODEW: dmSize at 68; fields after the name */
    WORD size = *(WORD *)(b + 68), extra = *(WORD *)(b + 70);
    if (size < 188) size = 188;                             /* through dmDisplayFrequency */
    memset(b, 0, (size_t)size + extra);
    *(WORD *)(b + 64) = 0x0401;                             /* dmSpecVersion (DM_SPECVERSION) */
    *(WORD *)(b + 68) = size;
    *(WORD *)(b + 70) = extra;
    for (int i = 0; DISPLAY_NAME[i]; i++) ((WCHAR *)b)[i] = (WCHAR)DISPLAY_NAME[i];
    *(DWORD *)(b + 72) = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFLAGS | DM_DISPLAYFREQUENCY;
    *(DWORD *)(b + 168) = m[2];                             /* dmBitsPerPel */
    *(DWORD *)(b + 172) = m[0];                             /* dmPelsWidth */
    *(DWORD *)(b + 176) = m[1];                             /* dmPelsHeight */
    *(DWORD *)(b + 184) = m[3];                             /* dmDisplayFrequency */
    return TRUE;
}
USERAPI BOOL EnumDisplaySettingsExW(LPCWSTR dev, DWORD mode, void *dm, DWORD flags)
{
    (void)flags;
    return EnumDisplaySettingsW(dev, mode, dm);
}
USERAPI BOOL EnumDisplaySettingsA(LPCSTR dev, DWORD mode, void *dm)
{
    (void)dev;
    DWORD m[4];
    if (!dm || !display_mode(mode, m)) return FALSE;
    BYTE *b = dm;                                           /* DEVMODEA: dmSize at 36 */
    WORD size = *(WORD *)(b + 36), extra = *(WORD *)(b + 38);
    if (size < 124) size = 124;                             /* through dmDisplayFrequency */
    memset(b, 0, (size_t)size + extra);
    memcpy(b, DISPLAY_NAME, sizeof(DISPLAY_NAME));
    *(WORD *)(b + 32) = 0x0401;
    *(WORD *)(b + 36) = size;
    *(WORD *)(b + 38) = extra;
    *(DWORD *)(b + 40) = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFLAGS | DM_DISPLAYFREQUENCY;
    *(DWORD *)(b + 104) = m[2];
    *(DWORD *)(b + 108) = m[0];
    *(DWORD *)(b + 112) = m[1];
    *(DWORD *)(b + 120) = m[3];
    return TRUE;
}
USERAPI BOOL EnumDisplaySettingsExA(LPCSTR dev, DWORD mode, void *dm, DWORD flags)
{
    (void)flags;
    return EnumDisplaySettingsA(dev, mode, dm);
}
/* Switch the display mode (the kernel lays the desktop out again and sends
 * WM_DISPLAYCHANGE).  Fields the caller leaves out keep their current
 * values; NULL returns to the registry mode.  CDS_TEST only checks,
 * CDS_UPDATEREGISTRY makes the mode the default, CDS_FULLSCREEN lasts
 * until the program ends. */
static LONG change_display(DWORD fields, DWORD bpp, DWORD w, DWORD h, DWORD hz, DWORD flags)
{
    DWORD cur[4];
    if (!display_mode((DWORD)-1, cur)) return -1;           /* DISP_CHANGE_FAILED */
    if ((fields & DM_BITSPERPEL) && bpp && bpp != 32) return -2;            /* DISP_CHANGE_BADMODE */
    if ((fields & DM_DISPLAYFREQUENCY) && hz > 1 && hz != cur[3]) return -2;
    INT32 in[3] = { (INT32)((fields & DM_PELSWIDTH) ? w : cur[0]),
                    (INT32)((fields & DM_PELSHEIGHT) ? h : cur[1]), (INT32)flags };
    return (LONG)NtNovaGuiCtl(0, CTL_SET_DISPLAY, 0, in);
}
USERAPI LONG ChangeDisplaySettingsExW(LPCWSTR d, void *dm, HWND h, DWORD f, void *p)
{
    (void)d; (void)h; (void)p;
    if (!dm) {                                              /* back to the registry mode */
        INT32 in[3] = { 0, 0, (INT32)f };
        return (LONG)NtNovaGuiCtl(0, CTL_SET_DISPLAY, 0, in);
    }
    const BYTE *b = dm;
    return change_display(*(const DWORD *)(b + 72), *(const DWORD *)(b + 168), *(const DWORD *)(b + 172),
                          *(const DWORD *)(b + 176), *(const DWORD *)(b + 184), f);
}
USERAPI LONG ChangeDisplaySettingsW(void *dm, DWORD f) { return ChangeDisplaySettingsExW(NULL, dm, NULL, f, NULL); }
USERAPI LONG ChangeDisplaySettingsExA(LPCSTR d, void *dm, HWND h, DWORD f, void *p)
{
    (void)d; (void)h; (void)p;
    if (!dm) return ChangeDisplaySettingsExW(NULL, NULL, NULL, f, NULL);
    const BYTE *b = dm;
    return change_display(*(const DWORD *)(b + 40), *(const DWORD *)(b + 104), *(const DWORD *)(b + 108),
                          *(const DWORD *)(b + 112), *(const DWORD *)(b + 120), f);
}
USERAPI LONG ChangeDisplaySettingsA(void *dm, DWORD f) { return ChangeDisplaySettingsExA(NULL, dm, NULL, f, NULL); }
