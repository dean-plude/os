/*
 * filedlg.h — the file dialog shared by GetOpenFileName/GetSaveFileName
 * (filedlg.c) and the IFileOpenDialog/IFileSaveDialog objects (ifiledlg.c).
 */
#pragma once
#include <windows.h>

/* FOS_* options (IFileDialog); GetOpenFileName's OFN_* flags map onto them */
#define FOS_OVERWRITEPROMPT   0x00000002
#define FOS_STRICTFILETYPES   0x00000004
#define FOS_NOCHANGEDIR       0x00000008
#define FOS_PICKFOLDERS       0x00000020
#define FOS_FORCEFILESYSTEM   0x00000040
#define FOS_NOVALIDATE        0x00000100
#define FOS_ALLOWMULTISELECT  0x00000200
#define FOS_PATHMUSTEXIST     0x00000800
#define FOS_FILEMUSTEXIST     0x00001000
#define FOS_CREATEPROMPT      0x00002000
#define FOS_NOREADONLYRETURN  0x00008000
#define FOS_FORCESHOWHIDDEN   0x10000000

#define FD_NAMES 32768                  /* characters of chosen names kept */

typedef struct { WCHAR *name, *spec; } FdFilter;

typedef struct FdEntry FdEntry;

typedef struct FileDlg FileDlg;
struct FileDlg {
    /* set by the caller */
    HWND owner;
    BOOL save;
    DWORD fos;
    WCHAR title[256], ok_label[64], name_label[64];
    FdFilter *filters;                  /* the caller's: stays valid while the dialog runs */
    int nfilters, filter;               /* filter: the chosen one, 0-based */
    WCHAR defext[32];                   /* without the dot */
    WCHAR dir[MAX_PATH];                /* the folder shown first; "" = the current directory */
    WCHAR name[MAX_PATH];               /* the file name shown first */
    /* the caller's hooks (IFileDialogEvents); on_ok returning FALSE keeps the dialog open */
    BOOL (*on_ok)(FileDlg *);
    void (*on_folder)(FileDlg *);
    void (*on_type)(FileDlg *);
    void (*on_sel)(FileDlg *);
    void *ctx;

    /* while it runs */
    HWND hwnd;
    WCHAR cur[MAX_PATH];                /* the folder listed; "" = the drives */
    WCHAR custom[MAX_PATH];             /* a wildcard the user typed, in place of the filter */
    FdEntry *ents;
    int nents, cap;

    /* the result: a folder and the names chosen in it */
    WCHAR res_dir[MAX_PATH];
    WCHAR res_names[FD_NAMES];          /* name\0name\0\0 */
    int res_count;
};

/* runs the dialog: TRUE when the user chose, FALSE when cancelled */
BOOL fd_run(FileDlg *d);
/* the text in the file name box (or d->name before and after it runs) */
void fd_get_name(FileDlg *d, WCHAR *out, int cap);
void fd_set_name(FileDlg *d, const WCHAR *name);
void fd_set_folder(FileDlg *d, const WCHAR *dir);
void fd_set_filter(FileDlg *d, int index);
/* the full path of the Nth result; FALSE past the end */
BOOL fd_result(FileDlg *d, int n, WCHAR *out);
/* the extension to add to a name without one ("" for none) */
void fd_default_ext(FileDlg *d, WCHAR *out);
void fd_close(FileDlg *d, int code);
