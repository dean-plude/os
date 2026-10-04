/*
 * netapi32.dll — accounts and groups (Net*), domain controllers (Ds*).
 *
 * A standalone machine with one account, the NovaOS user, a member of the
 * local Administrators and Users groups; no domain, no network shares.
 */
#include <windows.h>

#define NETAPI __declspec(dllexport)
#define NERR_Success         0
#define NERR_UserNotFound    2221
#define NERR_GroupNotFound   2220
#define NERR_UseNotFound     2250
#define NERR_InvalidComputer 2351
#define ERROR_NO_SUCH_DOMAIN_ 1355
#ifndef ERROR_INVALID_LEVEL
#define ERROR_INVALID_LEVEL 124
#endif
typedef DWORD NET_API_STATUS;

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);

static int wlen(const WCHAR *s) { int n = 0; if (s) while (s[n]) n++; return n; }
static int weq(const WCHAR *a, const WCHAR *b)
{
    for (; *a && *b; a++, b++) {
        WCHAR x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return !*a && !*b;
}

NETAPI NET_API_STATUS WINAPI NetApiBufferAllocate(DWORD n, LPVOID *buf)
{
    *buf = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n ? n : 1);
    return *buf ? NERR_Success : ERROR_NOT_ENOUGH_MEMORY;
}
NETAPI NET_API_STATUS WINAPI NetApiBufferFree(LPVOID buf) { if (buf) HeapFree(GetProcessHeap(), 0, buf); return NERR_Success; }

static void user_name(WCHAR *out, DWORD cap)
{
    DWORD n = cap;
    if (!GetUserNameW(out, &n)) lstrcpyW(out, L"User");
}

/* A buffer of @head bytes of structure followed by room for strings */
typedef struct { BYTE *base, *tail; } Pack;
static BOOL pack_new(Pack *p, DWORD head, DWORD strings)
{
    if (NetApiBufferAllocate(head + strings, (LPVOID *)&p->base)) return FALSE;
    p->tail = p->base + ((head + 7) & ~7u);
    return TRUE;
}
static LPWSTR pack_str(Pack *p, const WCHAR *s)
{
    int n = wlen(s);
    LPWSTR w = (LPWSTR)p->tail;
    memcpy(w, s, 2 * (size_t)(n + 1));
    p->tail += 2 * (n + 1);
    return w;
}

static const WCHAR *const g_local_groups[] = { L"Administrators", L"Users" };
static const WCHAR *const g_local_group_comments[] = {
    L"Administrators have complete and unrestricted access to the computer/domain",
    L"Users are prevented from making accidental or intentional system-wide changes" };

typedef struct {                                         /* USER_INFO_3 */
    LPWSTR name, password; DWORD password_age, priv; LPWSTR home_dir, comment; DWORD flags; LPWSTR script_path;
    DWORD auth_flags; LPWSTR full_name, usr_comment, parms, workstations; DWORD last_logon, last_logoff,
    acct_expires, max_storage, units_per_week; PBYTE logon_hours; DWORD bad_pw_count, num_logons;
    LPWSTR logon_server; DWORD country_code, code_page, user_id, primary_group_id; LPWSTR profile, home_dir_drive;
    DWORD password_expired;
} UI3;

NETAPI NET_API_STATUS WINAPI NetUserGetInfo(LPCWSTR server, LPCWSTR user, DWORD level, LPBYTE *buf)
{
    (void)server;
    WCHAR me[128];
    user_name(me, 128);
    *buf = 0;
    if (!user || !weq(user, me)) return NERR_UserNotFound;
    Pack p;
    switch (level) {
    case 0: case 10: case 1: case 2: case 3: case 4: case 11: case 20: case 23: {
        /* every level starts with the name; the larger ones are given as USER_INFO_3/4 */
        if (!pack_new(&p, sizeof(UI3) + 64, 1024)) return ERROR_NOT_ENOUGH_MEMORY;
        UI3 *u = (UI3 *)p.base;
        if (level == 0) { *(LPWSTR *)p.base = pack_str(&p, me); break; }
        if (level == 10 || level == 20 || level == 23) {    /* name, comment/full name... */
            LPWSTR *s = (LPWSTR *)p.base;
            s[0] = pack_str(&p, me);
            s[1] = pack_str(&p, level == 10 ? L"" : me);    /* 10: comment; 20/23: full_name */
            s[2] = pack_str(&p, level == 10 ? L"" : L""); /* 10: usr_comment; 20/23: comment */
            s[3] = pack_str(&p, me);                       /* 10: full_name */
            if (level == 20 || level == 23) { ((DWORD *)(s + 3))[0] = 0x10201; ((DWORD *)(s + 3))[1] = 1001; }
            break;
        }
        u->name = pack_str(&p, me);
        u->password = 0;
        u->priv = 2;                                        /* USER_PRIV_ADMIN */
        u->home_dir = pack_str(&p, L"");
        u->comment = pack_str(&p, L"");
        u->flags = 0x10201;                                 /* SCRIPT | NORMAL_ACCOUNT | DONT_EXPIRE_PASSWD */
        u->script_path = pack_str(&p, L"");
        u->full_name = pack_str(&p, me);
        u->usr_comment = pack_str(&p, L"");
        u->parms = pack_str(&p, L"");
        u->workstations = pack_str(&p, L"");
        u->acct_expires = 0xFFFFFFFF;                       /* TIMEQ_FOREVER */
        u->max_storage = 0xFFFFFFFF;
        u->units_per_week = 168;
        u->logon_server = pack_str(&p, L"\\\\*");
        u->user_id = 1001;
        u->primary_group_id = 513;
        u->profile = pack_str(&p, L"");
        u->home_dir_drive = pack_str(&p, L"");
        break;
    }
    case 24: {                                              /* USER_INFO_24: not a Microsoft account */
        if (!pack_new(&p, 48, 16)) return ERROR_NOT_ENOUGH_MEMORY;
        break;
    }
    default:
        return ERROR_INVALID_LEVEL;
    }
    *buf = p.base;
    return NERR_Success;
}

/* Lists of names (level 0 of the various enumerations) */
static NET_API_STATUS name_list(const WCHAR *const *names, const WCHAR *const *comments, int n, DWORD level,
                                LPBYTE *buf, LPDWORD read, LPDWORD total)
{
    DWORD per = level ? 2 : 1;
    Pack p;
    if (!pack_new(&p, (DWORD)(n * per * sizeof(LPWSTR)), 2048)) return ERROR_NOT_ENOUGH_MEMORY;
    LPWSTR *s = (LPWSTR *)p.base;
    for (int i = 0; i < n; i++) {
        s[i * per] = pack_str(&p, names[i]);
        if (per == 2) s[i * per + 1] = pack_str(&p, comments ? comments[i] : L"");
    }
    *buf = p.base;
    if (read) *read = (DWORD)n;
    if (total) *total = (DWORD)n;
    return NERR_Success;
}

NETAPI NET_API_STATUS WINAPI NetUserGetLocalGroups(LPCWSTR server, LPCWSTR user, DWORD level, DWORD flags, LPBYTE *buf,
                                                   DWORD pref, LPDWORD read, LPDWORD total)
{
    (void)server; (void)user; (void)flags; (void)pref;
    if (level) return ERROR_INVALID_LEVEL;
    return name_list(g_local_groups, 0, 2, 0, buf, read, total);
}
NETAPI NET_API_STATUS WINAPI NetUserGetGroups(LPCWSTR server, LPCWSTR user, DWORD level, LPBYTE *buf, DWORD pref,
                                              LPDWORD read, LPDWORD total)
{
    (void)server; (void)user; (void)pref;
    static const WCHAR *const none[] = { L"None" };
    if (level > 1) return ERROR_INVALID_LEVEL;
    return name_list(none, 0, 1, 0, buf, read, total);
}
NETAPI NET_API_STATUS WINAPI NetLocalGroupEnum(LPCWSTR server, DWORD level, LPBYTE *buf, DWORD pref, LPDWORD read,
                                               LPDWORD total, PDWORD_PTR resume)
{
    (void)server; (void)pref;
    if (level > 1) return ERROR_INVALID_LEVEL;
    if (resume && *resume) { *buf = 0; *read = *total = 0; return NERR_Success; }
    if (resume) *resume = 0;
    return name_list(g_local_groups, g_local_group_comments, 2, level, buf, read, total);
}
NETAPI NET_API_STATUS WINAPI NetLocalGroupGetInfo(LPCWSTR server, LPCWSTR group, DWORD level, LPBYTE *buf)
{
    (void)server;
    for (int i = 0; i < 2; i++)
        if (group && weq(group, g_local_groups[i])) {
            DWORD rd, tot;
            return name_list(&g_local_groups[i], &g_local_group_comments[i], 1, level ? 1 : 0, buf, &rd, &tot);
        }
    *buf = 0;
    return NERR_GroupNotFound;
}
NETAPI NET_API_STATUS WINAPI NetGroupEnum(LPCWSTR server, DWORD level, LPBYTE *buf, DWORD pref, LPDWORD read,
                                          LPDWORD total, PDWORD_PTR resume)
{
    (void)server; (void)level; (void)pref; (void)resume;
    *buf = 0; *read = 0; *total = 0;                       /* no global groups on a workstation */
    return NERR_Success;
}
NETAPI NET_API_STATUS WINAPI NetUserEnum(LPCWSTR server, DWORD level, DWORD filter, LPBYTE *buf, DWORD pref,
                                         LPDWORD read, LPDWORD total, PDWORD resume)
{
    (void)server; (void)filter; (void)pref;
    if (resume && *resume) { *buf = 0; *read = *total = 0; return NERR_Success; }
    WCHAR me[128];
    user_name(me, 128);
    const WCHAR *names[1] = { me };
    if (level == 0) return name_list(names, 0, 1, 0, buf, read, total);
    LPBYTE one;
    NET_API_STATUS s = NetUserGetInfo(0, me, level, &one);
    if (s) return s;
    *buf = one; *read = *total = 1;
    return NERR_Success;
}
NETAPI NET_API_STATUS WINAPI NetUseGetInfo(LPCWSTR server, LPCWSTR name, DWORD level, LPBYTE *buf)
{
    (void)server; (void)name; (void)level;
    *buf = 0;
    return NERR_UseNotFound;                                /* no network drives */
}
NETAPI NET_API_STATUS WINAPI NetWkstaGetInfo(LPCWSTR server, DWORD level, LPBYTE *buf)
{
    (void)server;
    if (level != 100) { *buf = 0; return ERROR_INVALID_LEVEL; }
    Pack p;                                                 /* WKSTA_INFO_100 */
    if (!pack_new(&p, 40, 256)) return ERROR_NOT_ENOUGH_MEMORY;
    DWORD *d = (DWORD *)p.base;
    d[0] = 500;                                             /* PLATFORM_ID_NT */
    WCHAR name[64];
    DWORD n = 64;
    if (!GetComputerNameW(name, &n)) lstrcpyW(name, L"NOVA-PC");
    *(LPWSTR *)(p.base + 8) = pack_str(&p, name);
    *(LPWSTR *)(p.base + 8 + sizeof(LPWSTR)) = pack_str(&p, L"WORKGROUP");
    ((DWORD *)(p.base + 8 + 2 * sizeof(LPWSTR)))[0] = 10;   /* ver_major */
    *buf = p.base;
    return NERR_Success;
}

/* No domain */
NETAPI DWORD WINAPI DsGetDcNameW(LPCWSTR comp, LPCWSTR dom, GUID *guid, LPCWSTR site, ULONG flags, PVOID *info)
{ (void)comp; (void)dom; (void)guid; (void)site; (void)flags; *info = 0; return ERROR_NO_SUCH_DOMAIN_; }
NETAPI DWORD WINAPI DsGetDcNameA(LPCSTR comp, LPCSTR dom, GUID *guid, LPCSTR site, ULONG flags, PVOID *info)
{ (void)comp; (void)dom; (void)guid; (void)site; (void)flags; *info = 0; return ERROR_NO_SUCH_DOMAIN_; }
NETAPI DWORD WINAPI DsEnumerateDomainTrustsW(LPWSTR server, ULONG flags, PVOID *doms, PULONG n)
{ (void)server; (void)flags; *doms = 0; *n = 0; return ERROR_NO_SUCH_DOMAIN_; }
NETAPI NET_API_STATUS WINAPI NetGetDCName(LPCWSTR server, LPCWSTR dom, LPBYTE *buf) { (void)server; (void)dom; *buf = 0; return 2453; /* NERR_DCNotFound */ }
/* not joined to Azure AD (Entra ID): no information, S_OK, as on a
 * workgroup PC */
NETAPI HRESULT WINAPI NetGetAadJoinInformation(LPCWSTR tenant, PVOID *info)
{
    (void)tenant;
    if (!info) return E_INVALIDARG;
    *info = 0;
    return S_OK;
}
NETAPI VOID WINAPI NetFreeAadJoinInformation(PVOID info) { (void)info; }

NETAPI NET_API_STATUS WINAPI NetGetJoinInformation(LPCWSTR server, LPWSTR *name, PDWORD status)
{
    (void)server;
    Pack p;
    if (!pack_new(&p, 0, 32)) return ERROR_NOT_ENOUGH_MEMORY;
    *name = pack_str(&p, L"WORKGROUP");
    *status = 2;                                            /* NetSetupWorkgroupName */
    return NERR_Success;
}
