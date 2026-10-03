/*
 * dhcpcsvc.dll — the DHCP client API.  NovaOS's network stack keeps its
 * DHCP lease in the kernel and does not ask the server for extra options,
 * so DhcpRequestParams finds none (a browser looking for a WPAD proxy URL
 * in option 252 moves on to its next method).
 */
#include <windows.h>

#define DHCPAPI __declspec(dllexport)

typedef struct { ULONG Flags; ULONG OptionId; BOOL IsVendor; LPBYTE Data; DWORD nBytesData; } DHCPAPI_PARAMS_;
typedef struct { ULONG nParams; DHCPAPI_PARAMS_ *Params; } DHCPCAPI_PARAMS_ARRAY_;

DHCPAPI DWORD WINAPI DhcpCApiInitialize(LPDWORD version) { if (version) *version = 2; return ERROR_SUCCESS; }
DHCPAPI VOID WINAPI DhcpCApiCleanup(void) {}

DHCPAPI DWORD WINAPI DhcpRequestParams(DWORD flags, LPVOID reserved, LPWSTR adapter, void *classid,
                                       DHCPCAPI_PARAMS_ARRAY_ send, DHCPCAPI_PARAMS_ARRAY_ recv, LPBYTE buf,
                                       LPDWORD size, LPWSTR app)
{
    (void)flags; (void)reserved; (void)adapter; (void)classid; (void)send; (void)buf; (void)app;
    for (ULONG i = 0; i < recv.nParams && recv.Params; i++) {
        recv.Params[i].Data = NULL;
        recv.Params[i].nBytesData = 0;
    }
    if (size) *size = 0;
    return ERROR_FILE_NOT_FOUND;                    /* no such option from the server */
}

DHCPAPI DWORD WINAPI DhcpUndoRequestParams(DWORD flags, LPVOID reserved, LPWSTR adapter, LPWSTR app)
{
    (void)flags; (void)reserved; (void)adapter; (void)app;
    return ERROR_SUCCESS;
}
