/*
 * oleacc.dll — Microsoft Active Accessibility.  No assistive technology
 * runs on NovaOS to ask windows for their accessible objects, so the
 * calls a window makes when it answers WM_GETOBJECT have no client: the
 * standard object cannot be built, and an object passed back yields 0
 * (the message's "not handled" answer).
 */
#include <objbase.h>

#define OLEACCAPI __declspec(dllexport)

OLEACCAPI LRESULT WINAPI LresultFromObject(REFIID iid, WPARAM wp, IUnknown *obj) { (void)iid; (void)wp; (void)obj; return 0; }
OLEACCAPI HRESULT WINAPI ObjectFromLresult(LRESULT r, REFIID iid, WPARAM wp, void **out) { (void)r; (void)iid; (void)wp; if (out) *out = 0; return E_INVALIDARG; }
OLEACCAPI HRESULT WINAPI CreateStdAccessibleObject(HWND h, LONG id, REFIID iid, void **out) { (void)h; (void)id; (void)iid; if (out) *out = 0; return E_NOTIMPL; }
OLEACCAPI HRESULT WINAPI AccessibleObjectFromWindow(HWND h, DWORD id, REFIID iid, void **out) { (void)h; (void)id; (void)iid; if (out) *out = 0; return E_NOTIMPL; }
OLEACCAPI HRESULT WINAPI AccessibleChildren(IUnknown *acc, LONG start, LONG n, void *children, LONG *got) { (void)acc; (void)start; (void)n; (void)children; if (got) *got = 0; return E_NOTIMPL; }
OLEACCAPI HANDLE WINAPI GetProcessHandleFromHwnd(HWND h)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid ? OpenProcess(0x1F0FFF /* PROCESS_ALL_ACCESS */, FALSE, pid) : 0;
}
