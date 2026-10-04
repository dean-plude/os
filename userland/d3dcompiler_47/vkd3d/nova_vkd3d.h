/* Included before every vkd3d-shader source (-include): what NovaOS's
 * Windows headers leave out and vkd3d-shader's Windows build expects from
 * the SDK's */
#include <windows.h>
#include <stddef.h>
#include <string.h>

#ifndef DXGI_ERROR_NOT_FOUND
#define DXGI_ERROR_NOT_FOUND      ((HRESULT)0x887a0002)
#define DXGI_ERROR_MORE_DATA      ((HRESULT)0x887a0003)
#define DXGI_ERROR_UNSUPPORTED    ((HRESULT)0x887a0004)
#define DXGI_ERROR_ALREADY_EXISTS ((HRESULT)0x887a0036)
#endif
#ifndef CONTAINING_RECORD
#define CONTAINING_RECORD(address, type, field) ((type *)((char *)(address) - offsetof(type, field)))
#endif

/* windows.h already has IUnknown: keep vkd3d_d3dcommon.h from declaring it again */
#define __IUnknown_FWD_DEFINED__
#define __IUnknown_INTERFACE_DEFINED__
