/*
 * parent.c — the shell namespace calls that start from item ID lists.
 * NovaOS's shell32 has no namespace folders (SHGetDesktopFolder fails),
 * so there is nothing to bind to (item arrays: shellitem.c).
 */
#include <windows.h>
#include <shellapi.h>

#define SHAPI __declspec(dllexport)

SHAPI HRESULT WINAPI SHBindToParent(const void *pidl, REFIID riid, void **out, const void **last)
{
    (void)pidl; (void)riid;
    if (out) *out = 0;
    if (last) *last = 0;
    return pidl ? E_NOTIMPL : E_INVALIDARG;
}
