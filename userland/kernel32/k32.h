/* k32.h — helpers shared by the kernel32 sources (not exported) */
#pragma once
#include <winternl.h>

typedef struct { WCHAR buf[MAX_PATH + 8]; UNICODE_STRING us; OBJECT_ATTRIBUTES oa; } NtPath;

BOOL        fail_status(NTSTATUS s);
int         ieq(const char *a, const char *b);
int         w2u(const WCHAR *w, int n, char *out, int cap);   /* UTF-16 -> UTF-8 */
int         u2w(const char *s, int n, WCHAR *out, int cap);   /* UTF-8 -> UTF-16 */
const char *cwd(void);
int         full_path(const char *name, char *out, int cap);
BOOL        nt_path(const char *name, NtPath *p);
char       *wide_to_temp(LPCWSTR w, char *buf, int cap);
const char *k32_module_path(HMODULE m, char *tmp);   /* tmp: MAX_PATH; 0 if not loaded */

/* extra.c */
void k32_forget_handle(HANDLE h);
void k32_io_done(HANDLE h, OVERLAPPED *o, NTSTATUS s, DWORD bytes);

/* compat.c */
BOOL k32_close_snapshot(HANDLE h);
BOOL k32_find_close_stream(HANDLE h);
