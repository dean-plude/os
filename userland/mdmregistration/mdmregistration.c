/*
 * mdmregistration.dll — enrolment in mobile device management (Intune and
 * the like).  A NovaOS machine is never enrolled, which is what programs
 * that check before applying managed policy (Microsoft Edge Update) ask.
 */
#include <windows.h>

#define MDMAPI __declspec(dllexport)

MDMAPI HRESULT WINAPI IsDeviceRegisteredWithManagement(BOOL *registered, DWORD n, LPWSTR upn)
{
    if (!registered) return E_INVALIDARG;
    *registered = FALSE;
    if (upn && n) upn[0] = 0;
    return S_OK;
}

MDMAPI HRESULT WINAPI IsManagementRegistrationAllowed(BOOL *allowed)
{
    if (!allowed) return E_INVALIDARG;
    *allowed = TRUE;
    return S_OK;
}

MDMAPI HRESULT WINAPI IsMdmUxWithoutAadAllowed(BOOL *allowed)
{
    if (!allowed) return E_INVALIDARG;
    *allowed = FALSE;
    return S_OK;
}

/* MANAGEMENT_SERVICE_INFO lists: there are no management services */
MDMAPI HRESULT WINAPI DiscoverManagementService(LPCWSTR upn, PVOID *info)
{
    (void)upn;
    if (!info) return E_INVALIDARG;
    *info = 0;
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

MDMAPI HRESULT WINAPI GetManagementAppHyperlink(DWORD n, LPWSTR link)
{
    if (link && n) link[0] = 0;
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}
