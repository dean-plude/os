/*
 * wldap32.dll — the LDAP client API.  NovaOS has no directory servers to
 * talk to and no LDAP protocol client yet: a session handle is made as
 * Windows makes one (ldap_init does not connect), options are kept on it,
 * and the calls that would reach a server (binds, searches) answer
 * LDAP_SERVER_DOWN, as Windows does when no server answers.  Programs that
 * only link to LDAP (curl's ldap:// support, for one) load and run.
 */
#include <windows.h>

#define LDAPAPI __declspec(dllexport)

#define LDAP_SUCCESS_          0x00
#define LDAP_PARAM_ERROR_      0x59
#define LDAP_SERVER_DOWN_      0x51
#define LDAP_NO_MEMORY_        0x5A
#define LDAP_PORT_             389
#define LDAP_SSL_PORT_         636
#define LDAP_OPT_VERSION_      0x11

/* The front of Windows' LDAP structure (ld_host, ld_version, ...) that
 * programs read; the rest is this DLL's own */
typedef struct Ldap {
    struct { UINT_PTR sb_sd; UCHAR reserved[41]; ULONG_PTR sb_naddr; UCHAR reserved2[24]; } sb;
    char *ld_host;
    ULONG ld_version;
    UCHAR ld_lberoptions;
    ULONG ld_deref, ld_timelimit, ld_sizelimit, ld_errno;
    char *ld_matched, *ld_error;
    ULONG ld_msgid;
    UCHAR reserved3[25];
    ULONG ld_cldaptries, ld_cldaptimeout, ld_refhoplimit, ld_options;
    /* NovaOS's own */
    ULONG port, ssl;
} Ldap;

static ULONG fail(Ldap *ld, ULONG err)
{
    if (ld) ld->ld_errno = err;
    return err;
}

static Ldap *open_session(const char *host, ULONG port, ULONG ssl)
{
    Ldap *ld = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*ld));
    if (!ld) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    if (host) {
        SIZE_T n = lstrlenA(host) + 1;
        ld->ld_host = HeapAlloc(GetProcessHeap(), 0, n);
        if (ld->ld_host) CopyMemory(ld->ld_host, host, n);
    }
    ld->ld_version = 2;
    ld->port = port ? port : ssl ? LDAP_SSL_PORT_ : LDAP_PORT_;
    ld->ssl = ssl;
    return ld;
}

static char *narrow(const WCHAR *w)
{
    if (!w) return NULL;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = HeapAlloc(GetProcessHeap(), 0, n ? n : 1);
    if (s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

/* Sessions: made without connecting, as on Windows */
LDAPAPI Ldap *__cdecl ldap_initA(const char *host, ULONG port) { return open_session(host, port, 0); }
LDAPAPI Ldap *__cdecl ldap_init(const char *host, ULONG port) { return open_session(host, port, 0); }
LDAPAPI Ldap *__cdecl ldap_openA(const char *host, ULONG port) { (void)host; (void)port; SetLastError(LDAP_SERVER_DOWN_); return NULL; }
LDAPAPI Ldap *__cdecl ldap_sslinitA(const char *host, ULONG port, int secure) { return open_session(host, port, secure != 0); }
LDAPAPI Ldap *__cdecl ldap_sslinit(const char *host, ULONG port, int secure) { return open_session(host, port, secure != 0); }
LDAPAPI Ldap *__cdecl ldap_initW(const WCHAR *host, ULONG port)
{
    char *h = narrow(host);
    Ldap *ld = open_session(h, port, 0);
    if (h) HeapFree(GetProcessHeap(), 0, h);
    return ld;
}
LDAPAPI Ldap *__cdecl ldap_sslinitW(const WCHAR *host, ULONG port, int secure)
{
    char *h = narrow(host);
    Ldap *ld = open_session(h, port, secure != 0);
    if (h) HeapFree(GetProcessHeap(), 0, h);
    return ld;
}
LDAPAPI ULONG __cdecl ldap_unbind(Ldap *ld)
{
    if (!ld) return LDAP_PARAM_ERROR_;
    if (ld->ld_host) HeapFree(GetProcessHeap(), 0, ld->ld_host);
    HeapFree(GetProcessHeap(), 0, ld);
    return LDAP_SUCCESS_;
}
LDAPAPI ULONG __cdecl ldap_unbind_s(Ldap *ld) { return ldap_unbind(ld); }

/* Options kept on the session (the protocol version is the one programs set) */
LDAPAPI ULONG __cdecl ldap_set_optionA(Ldap *ld, int opt, const void *val)
{
    if (!ld) return LDAP_PARAM_ERROR_;
    if (opt == LDAP_OPT_VERSION_ && val) ld->ld_version = *(const ULONG *)val;
    return LDAP_SUCCESS_;
}
LDAPAPI ULONG __cdecl ldap_set_option(Ldap *ld, int opt, const void *val) { return ldap_set_optionA(ld, opt, val); }
LDAPAPI ULONG __cdecl ldap_set_optionW(Ldap *ld, int opt, const void *val) { return ldap_set_optionA(ld, opt, val); }
LDAPAPI ULONG __cdecl ldap_get_optionA(Ldap *ld, int opt, void *val)
{
    if (!ld || !val) return LDAP_PARAM_ERROR_;
    if (opt == LDAP_OPT_VERSION_) { *(ULONG *)val = ld->ld_version; return LDAP_SUCCESS_; }
    return LDAP_PARAM_ERROR_;
}
LDAPAPI ULONG __cdecl ldap_get_option(Ldap *ld, int opt, void *val) { return ldap_get_optionA(ld, opt, val); }
LDAPAPI ULONG __cdecl ldap_get_optionW(Ldap *ld, int opt, void *val) { return ldap_get_optionA(ld, opt, val); }

/* What would reach a server: none answers */
LDAPAPI ULONG __cdecl ldap_bind_s(Ldap *ld, const char *dn, const char *cred, ULONG method)
{ (void)dn; (void)cred; (void)method; return ld ? fail(ld, LDAP_SERVER_DOWN_) : LDAP_PARAM_ERROR_; }
LDAPAPI ULONG __cdecl ldap_bind_sA(Ldap *ld, const char *dn, const char *cred, ULONG method) { return ldap_bind_s(ld, dn, cred, method); }
LDAPAPI ULONG __cdecl ldap_bind_sW(Ldap *ld, const WCHAR *dn, const WCHAR *cred, ULONG method)
{ (void)dn; (void)cred; return ldap_bind_s(ld, NULL, NULL, method); }
LDAPAPI ULONG __cdecl ldap_simple_bind_s(Ldap *ld, const char *dn, const char *pw) { return ldap_bind_s(ld, dn, pw, 0x80); }
LDAPAPI ULONG __cdecl ldap_simple_bind_sA(Ldap *ld, const char *dn, const char *pw) { return ldap_bind_s(ld, dn, pw, 0x80); }
LDAPAPI ULONG __cdecl ldap_simple_bind_sW(Ldap *ld, const WCHAR *dn, const WCHAR *pw) { (void)dn; (void)pw; return ldap_bind_s(ld, NULL, NULL, 0x80); }
LDAPAPI ULONG __cdecl ldap_search_s(Ldap *ld, const char *base, ULONG scope, const char *filter, char **attrs, ULONG only, void **res)
{
    (void)base; (void)scope; (void)filter; (void)attrs; (void)only;
    if (res) *res = NULL;
    return ld ? fail(ld, LDAP_SERVER_DOWN_) : LDAP_PARAM_ERROR_;
}
LDAPAPI ULONG __cdecl ldap_search_sA(Ldap *ld, const char *base, ULONG scope, const char *filter, char **attrs, ULONG only, void **res)
{ return ldap_search_s(ld, base, scope, filter, attrs, only, res); }
LDAPAPI ULONG __cdecl ldap_search_sW(Ldap *ld, const WCHAR *base, ULONG scope, const WCHAR *filter, WCHAR **attrs, ULONG only, void **res)
{ (void)base; (void)filter; (void)attrs; return ldap_search_s(ld, NULL, scope, NULL, NULL, only, res); }

/* Results: a search never returns any, so there is nothing to walk */
LDAPAPI void *__cdecl ldap_first_entry(Ldap *ld, void *res) { (void)res; if (ld) ld->ld_errno = LDAP_SUCCESS_; return NULL; }
LDAPAPI void *__cdecl ldap_next_entry(Ldap *ld, void *entry) { (void)entry; if (ld) ld->ld_errno = LDAP_SUCCESS_; return NULL; }
LDAPAPI ULONG __cdecl ldap_count_entries(Ldap *ld, void *res) { (void)ld; (void)res; return 0; }
LDAPAPI char *__cdecl ldap_first_attribute(Ldap *ld, void *entry, void **ber) { (void)ld; (void)entry; if (ber) *ber = NULL; return NULL; }
LDAPAPI char *__cdecl ldap_first_attributeA(Ldap *ld, void *entry, void **ber) { return ldap_first_attribute(ld, entry, ber); }
LDAPAPI WCHAR *__cdecl ldap_first_attributeW(Ldap *ld, void *entry, void **ber) { (void)ld; (void)entry; if (ber) *ber = NULL; return NULL; }
LDAPAPI char *__cdecl ldap_next_attribute(Ldap *ld, void *entry, void *ber) { (void)ld; (void)entry; (void)ber; return NULL; }
LDAPAPI char *__cdecl ldap_next_attributeA(Ldap *ld, void *entry, void *ber) { return ldap_next_attribute(ld, entry, ber); }
LDAPAPI WCHAR *__cdecl ldap_next_attributeW(Ldap *ld, void *entry, void *ber) { (void)ld; (void)entry; (void)ber; return NULL; }
LDAPAPI char *__cdecl ldap_get_dn(Ldap *ld, void *entry) { (void)entry; if (ld) ld->ld_errno = LDAP_PARAM_ERROR_; return NULL; }
LDAPAPI char *__cdecl ldap_get_dnA(Ldap *ld, void *entry) { return ldap_get_dn(ld, entry); }
LDAPAPI WCHAR *__cdecl ldap_get_dnW(Ldap *ld, void *entry) { (void)entry; if (ld) ld->ld_errno = LDAP_PARAM_ERROR_; return NULL; }
LDAPAPI void **__cdecl ldap_get_values_len(Ldap *ld, void *entry, const char *attr) { (void)ld; (void)entry; (void)attr; return NULL; }
LDAPAPI void **__cdecl ldap_get_values_lenA(Ldap *ld, void *entry, const char *attr) { return ldap_get_values_len(ld, entry, attr); }
LDAPAPI void **__cdecl ldap_get_values_lenW(Ldap *ld, void *entry, const WCHAR *attr) { (void)ld; (void)entry; (void)attr; return NULL; }

/* Freeing: results and values are never handed out, so these only check */
LDAPAPI ULONG __cdecl ldap_msgfree(void *res) { (void)res; return LDAP_SUCCESS_; }
LDAPAPI ULONG __cdecl ldap_value_free_len(void **vals) { (void)vals; return LDAP_SUCCESS_; }
LDAPAPI void __cdecl ldap_memfree(char *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }
LDAPAPI void __cdecl ldap_memfreeA(char *p) { ldap_memfree(p); }
LDAPAPI void __cdecl ldap_memfreeW(WCHAR *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }
LDAPAPI void __cdecl ber_free(void *ber, int freebuf) { (void)freebuf; if (ber) HeapFree(GetProcessHeap(), 0, ber); }

/* Error text, as Windows words it */
static const struct { ULONG code; const char *a; const WCHAR *w; } errs[] = {
    { 0x00, "Success", L"Success" },
    { 0x51, "Server Down", L"Server Down" },
    { 0x59, "Bad Parameter to an ldap routine", L"Bad Parameter to an ldap routine" },
    { 0x5A, "Out of memory", L"Out of memory" },
};
LDAPAPI char *__cdecl ldap_err2stringA(ULONG err)
{
    for (unsigned i = 0; i < sizeof(errs) / sizeof(errs[0]); i++)
        if (errs[i].code == err) return (char *)errs[i].a;
    return (char *)"Unknown error";
}
LDAPAPI char *__cdecl ldap_err2string(ULONG err) { return ldap_err2stringA(err); }
LDAPAPI WCHAR *__cdecl ldap_err2stringW(ULONG err)
{
    for (unsigned i = 0; i < sizeof(errs) / sizeof(errs[0]); i++)
        if (errs[i].code == err) return (WCHAR *)errs[i].w;
    return (WCHAR *)L"Unknown error";
}
LDAPAPI ULONG __cdecl LdapGetLastError(void) { return GetLastError(); }
