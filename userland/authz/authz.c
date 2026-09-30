/*
 * authz.dll — access checks against a client context.  One user with full
 * access to everything: every check grants what is asked (as advapi32's
 * AccessCheck does).
 */
#include <windows.h>

#define AUTHZAPI __declspec(dllexport)

static int g_rm, g_ctx;                                  /* what the handles point at */

AUTHZAPI BOOL WINAPI AuthzInitializeResourceManager(DWORD flags, PVOID cb1, PVOID cb2, PVOID cb3, PCWSTR name, PHANDLE rm)
{ (void)flags; (void)cb1; (void)cb2; (void)cb3; (void)name; *rm = &g_rm; return TRUE; }
AUTHZAPI BOOL WINAPI AuthzFreeResourceManager(HANDLE rm) { (void)rm; return TRUE; }
AUTHZAPI BOOL WINAPI AuthzInitializeContextFromSid(DWORD flags, PSID sid, HANDLE rm, PLARGE_INTEGER exp, LUID id,
                                                   PVOID arg, PHANDLE ctx)
{ (void)flags; (void)sid; (void)rm; (void)exp; (void)id; (void)arg; *ctx = &g_ctx; return TRUE; }
AUTHZAPI BOOL WINAPI AuthzInitializeContextFromToken(DWORD flags, HANDLE token, HANDLE rm, PLARGE_INTEGER exp, LUID id,
                                                     PVOID arg, PHANDLE ctx)
{ (void)flags; (void)token; (void)rm; (void)exp; (void)id; (void)arg; *ctx = &g_ctx; return TRUE; }
AUTHZAPI BOOL WINAPI AuthzFreeContext(HANDLE ctx) { (void)ctx; return TRUE; }

/* AUTHZ_ACCESS_REQUEST { DesiredAccess, ... }; AUTHZ_ACCESS_REPLY
 * { ResultListLength, GrantedAccessMask*, SaclEvaluationResults*, Error* } */
AUTHZAPI BOOL WINAPI AuthzAccessCheck(DWORD flags, HANDLE ctx, PVOID request, HANDLE audit, PVOID sd, PVOID *sds,
                                      DWORD nsds, PVOID reply, PHANDLE cache)
{
    (void)flags; (void)ctx; (void)audit; (void)sd; (void)sds; (void)nsds;
    DWORD want = *(DWORD *)request;
    if (want & 0x02000000) want = 0x001F01FF;               /* MAXIMUM_ALLOWED: all file rights */
    struct { DWORD n; DWORD *granted, *sacl, *error; } *r = reply;
    for (DWORD i = 0; i < r->n; i++) {
        r->granted[i] = want;
        if (r->error) r->error[i] = 0;
    }
    if (cache) *cache = 0;
    return TRUE;
}
