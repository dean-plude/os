/*
 * msi.h — Windows Installer for NovaOS: the engine's interface (msi.dll
 * exports and msiexec.exe use it)
 */
#pragma once

#include <windows.h>
#include <stdbool.h>
#include <wchar.h>
#include <stdio.h>

/* UI levels (INSTALLUILEVEL) */
#define MSIUI_NONE    2          /* /qn: no UI at all */
#define MSIUI_BASIC   3          /* /qb: progress only */
#define MSIUI_REDUCED 4
#define MSIUI_FULL    5          /* progress, then a completion message */

/* Results (Win32 error codes) */
#define MSI_OK                    0
#define MSI_ERROR_USEREXIT        1602
#define MSI_ERROR_FAILURE         1603
#define MSI_ERROR_PACKAGE_OPEN    1619
#define MSI_ERROR_PACKAGE_INVALID 1620
#define MSI_ERROR_UNKNOWN_PRODUCT 1605
#define MSI_ERROR_TRANSFORM       1624
#define MSI_ERROR_PATCH_TARGET    1642

typedef struct {
    const WCHAR *package;        /* .msi path (install), or NULL */
    const WCHAR *product_code;   /* "{...}" (uninstall / reinstall of a registered product) */
    const WCHAR *properties;     /* "NAME=value NAME2="a b"" from the command line */
    bool         remove;         /* uninstall */
    int          ui_level;       /* MSIUI_* */
    const WCHAR *logfile;        /* /l*v FILE, or NULL */
    const WCHAR *patch;          /* /p PATCH.msp: apply it to the product it is for
                                    (with remove: /uninstall PATCH.msp takes it off) */
} MsiRequest;

/* Run an installation; @err (may be NULL) gets a description on failure */
#ifndef MSI_EXPORT
#define MSI_EXPORT
#endif
MSI_EXPORT int MsiRunInstall(const MsiRequest *req, char *err, int err_cap);
