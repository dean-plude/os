/* com_private.h — what ole32.c and marshal.c (COM between processes) share */
#pragma once
#include <objbase.h>

/* the calling thread's apartment: 1 = single-threaded (STA), 0 = the MTA
 * or none (ole32.c) */
int ole_thread_is_sta(void);

/* the apartment an object lives in: calls into it run on its thread (an
 * STA, reached through a hidden window's message queue) or on any thread
 * (the MTA: tid 0) */
typedef struct OleApt { DWORD tid; HWND wnd; } OleApt;
void ole_current_apt(OleApt *a);

/* a class object this process registered for other processes
 * (CoRegisterClassObject with CLSCTX_LOCAL_SERVER), AddRef'd, with the
 * apartment it was registered from; NULL when there is none (ole32.c) */
IUnknown *ole_local_class(REFCLSID clsid, OleApt *apt);

/* the pipe a registered class listens on for activation requests */
void *ole_class_listen(REFCLSID clsid);
void ole_class_unlisten(void *listener);

/* CoGetClassObject / CoCreateInstance with CLSCTX_LOCAL_SERVER for a class
 * no process here registered: HKCR\CLSID\{clsid}\LocalServer32 is started
 * with -Embedding when no running server serves it */
#define OLE_ACTIVATE_CREATE 0
#define OLE_ACTIVATE_CLASS  1
HRESULT ole_local_activate(REFCLSID clsid, DWORD mode, REFIID riid, void **ppv);
