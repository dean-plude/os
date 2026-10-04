/* qtwebtest.exe — the calls Qt WebEngine (Chromium, in GOG GALAXY's client)
 * imports at load time beyond what NovaOS had, each checked for the answer
 * Windows gives on a PC without the hardware they reach:
 *
 *   bthprops.cpl  no Bluetooth radio or device; the SDP record parsers
 *   d3d12.dll     no Direct3D 12 device or debug layer (by name and ordinal)
 *   winusb.dll    no WinUSB device handle
 *   hid.dll       no parsed report descriptor
 *   setupapi      no device instance or interface to open
 *   kernel32      SetEnvironmentStringsW
 *   advapi32      TreeResetNamedSecurityInfoW
 *   netapi32      NetShareEnum: no shares
 *   iphlpapi      eth0's name, LUID and GUID; no DHCP adapters
 *   dnsapi        DnsQueryEx, synchronous and with a completion routine
 *   ws2_32        WSAAccept with a condition function
 *   gdi32         SetArcDirection (Arc drawn clockwise), CancelDC
 *   userenv       CreateAppContainerProfile / DeleteAppContainerProfile
 *   crypt32       CryptVerifyCertificateSignatureEx, CertCompareCertificateName,
 *                 CertControlStore
 *   winhttp       the proxy resolver (WinHttpGetProxyForUrlEx)
 *   urlmon        the Internet security manager's zones
 *   ucrt          _ultow_s
 * Built 64- and 32-bit. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <errno.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

static int passed, failed;
static void check(const char *what, int ok)
{
    if (ok) passed++; else failed++;
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
}

/* -------- declarations (NovaOS's headers carry few of these) -------- */
typedef struct { DWORD dwSize; } BT_FIND_RADIO_PARAMS;
typedef struct {
    int type, specificType;
    union {
        struct { ULONGLONG lo; LONGLONG hi; } int128;
        LONGLONG int64; LONG int32; SHORT int16; CHAR int8;
        struct { ULONGLONG lo, hi; } uint128;
        ULONGLONG uint64; ULONG uint32; USHORT uint16; UCHAR uint8;
        UCHAR booleanVal;
        GUID uuid128; ULONG uuid32; USHORT uuid16;
        struct { LPBYTE value; ULONG length; } string, url, sequence, alternative;
    } data;
} SDP_ELEMENT_DATA;
__declspec(dllimport) HANDLE WINAPI BluetoothFindFirstRadio(const BT_FIND_RADIO_PARAMS *, HANDLE *);
__declspec(dllimport) HANDLE WINAPI BluetoothFindFirstDevice(const void *, void *);
__declspec(dllimport) BOOL WINAPI BluetoothIsConnectable(HANDLE);
__declspec(dllimport) DWORD WINAPI BluetoothSdpGetAttributeValue(LPBYTE, ULONG, USHORT, SDP_ELEMENT_DATA *);
__declspec(dllimport) DWORD WINAPI BluetoothSdpGetContainerElementData(LPBYTE, ULONG, HANDLE *, SDP_ELEMENT_DATA *);

__declspec(dllimport) HRESULT WINAPI D3D12CreateDevice(void *, UINT, REFIID, void **);
__declspec(dllimport) HRESULT WINAPI D3D12GetDebugInterface(REFIID, void **);

__declspec(dllimport) BOOL WINAPI WinUsb_Initialize(HANDLE, PVOID *);
__declspec(dllimport) LONG WINAPI HidP_GetValueCaps(int, PVOID, USHORT *, PVOID);
__declspec(dllimport) BOOL WINAPI SetupDiOpenDeviceInfoW(HANDLE, LPCWSTR, HWND, DWORD, PVOID);
__declspec(dllimport) HANDLE WINAPI SetupDiCreateDeviceInfoList(const GUID *, HWND);
__declspec(dllimport) BOOL WINAPI SetupDiDestroyDeviceInfoList(HANDLE);

WINBASEAPI BOOL WINAPI SetEnvironmentStringsW(LPWSTR);
typedef void (WINAPI *TREE_PROGRESS)(LPWSTR, DWORD, DWORD *, PVOID, BOOL);
__declspec(dllimport) DWORD WINAPI TreeResetNamedSecurityInfoW(LPWSTR, int, DWORD, PSID, PSID, PACL, PACL, BOOL, TREE_PROGRESS,
                                                               DWORD, PVOID);
__declspec(dllimport) DWORD WINAPI NetShareEnum(LPWSTR, DWORD, LPBYTE *, DWORD, LPDWORD, LPDWORD, LPDWORD);
__declspec(dllimport) DWORD WINAPI NetApiBufferFree(LPVOID);

__declspec(dllimport) DWORD WINAPI ConvertInterfaceNameToLuidW(LPCWSTR, ULONG64 *);
__declspec(dllimport) DWORD WINAPI ConvertInterfaceLuidToGuid(const ULONG64 *, GUID *);
__declspec(dllimport) DWORD WINAPI ConvertInterfaceGuidToLuid(const GUID *, ULONG64 *);
__declspec(dllimport) DWORD WINAPI ConvertInterfaceIndexToLuid(ULONG, ULONG64 *);
__declspec(dllimport) DWORD WINAPI GetInterfaceInfo(PVOID, PULONG);

typedef struct { ULONG Version; LONG QueryStatus; ULONG64 QueryOptions; PVOID pQueryRecords; PVOID Reserved; } DNS_QUERY_RESULT;
typedef VOID (WINAPI *DNS_COMPLETION)(PVOID, DNS_QUERY_RESULT *);
typedef struct {
    ULONG Version; PCWSTR QueryName; WORD QueryType; ULONG64 QueryOptions; PVOID pDnsServerList;
    ULONG InterfaceIndex; DNS_COMPLETION pQueryCompletionCallback; PVOID pQueryContext;
} DNS_QUERY_REQUEST;
__declspec(dllimport) LONG WINAPI DnsQueryEx(DNS_QUERY_REQUEST *, DNS_QUERY_RESULT *, PVOID);

typedef int (WINAPI *CONDITIONPROC)(LPWSABUF, LPWSABUF, void *, void *, LPWSABUF, LPWSABUF, unsigned int *, DWORD_PTR);
__declspec(dllimport) SOCKET WINAPI WSAAccept(SOCKET, struct sockaddr *, int *, CONDITIONPROC, DWORD_PTR);

__declspec(dllimport) int WINAPI SetArcDirection(HDC, int);
__declspec(dllimport) int WINAPI GetArcDirection(HDC);
__declspec(dllimport) BOOL WINAPI CancelDC(HDC);

__declspec(dllimport) HRESULT WINAPI CreateAppContainerProfile(LPCWSTR, LPCWSTR, LPCWSTR, PVOID, DWORD, PSID *);
__declspec(dllimport) HRESULT WINAPI DeleteAppContainerProfile(LPCWSTR);
__declspec(dllimport) HRESULT WINAPI DeriveAppContainerSidFromAppContainerName(LPCWSTR, PSID *);

typedef struct { DWORD cbData; BYTE *pbData; } CBLOB;
typedef struct { DWORD cbData; BYTE *pbData; DWORD cUnusedBits; } CBITS;
typedef struct { LPSTR pszObjId; CBLOB Parameters; } CALG;
typedef struct { CALG Algorithm; CBITS PublicKey; } CPUBKEY;
typedef struct {
    DWORD dwVersion; CBLOB SerialNumber; CALG SignatureAlgorithm; CBLOB Issuer; FILETIME NotBefore, NotAfter;
    CBLOB Subject; CPUBKEY SubjectPublicKeyInfo;
} CINFO;
typedef struct { DWORD dwCertEncodingType; BYTE *pbCertEncoded; DWORD cbCertEncoded; CINFO *pCertInfo; HANDLE hCertStore; } CCTX;
__declspec(dllimport) HANDLE WINAPI CertOpenSystemStoreW(ULONG_PTR, LPCWSTR);
__declspec(dllimport) BOOL WINAPI CertCloseStore(HANDLE, DWORD);
__declspec(dllimport) const CCTX *WINAPI CertEnumCertificatesInStore(HANDLE, const CCTX *);
__declspec(dllimport) const CCTX *WINAPI CertDuplicateCertificateContext(const CCTX *);
__declspec(dllimport) BOOL WINAPI CertFreeCertificateContext(const CCTX *);
__declspec(dllimport) BOOL WINAPI CertControlStore(HANDLE, DWORD, DWORD, const void *);
__declspec(dllimport) BOOL WINAPI CertCompareCertificateName(DWORD, CBLOB *, CBLOB *);
__declspec(dllimport) BOOL WINAPI CryptVerifyCertificateSignatureEx(ULONG_PTR, DWORD, DWORD, void *, DWORD, void *, DWORD, void *);

typedef struct { DWORD cEntries; void *pEntries; } PROXY_RESULT;
__declspec(dllimport) DWORD WINAPI WinHttpCreateProxyResolver(HINTERNET, HINTERNET *);
__declspec(dllimport) DWORD WINAPI WinHttpGetProxyForUrlEx(HINTERNET, LPCWSTR, LPVOID, DWORD_PTR);
__declspec(dllimport) DWORD WINAPI WinHttpGetProxyResult(HINTERNET, PROXY_RESULT *);
__declspec(dllimport) VOID WINAPI WinHttpFreeProxyResult(PROXY_RESULT *);

__declspec(dllimport) HRESULT WINAPI CoInternetCreateSecurityManager(void *, void **, DWORD);

_CRTIMP int _ultow_s(unsigned long, wchar_t *, size_t, int);
__declspec(dllimport) BOOL WINAPI Arc(HDC, int, int, int, int, int, int, int, int);
WINADVAPI BOOL WINAPI AddAccessAllowedAceEx(PACL, DWORD, DWORD, DWORD, PSID);
#define OBJECT_INHERIT_ACE_ 1
#define CONTAINER_INHERIT_ACE_ 2
#define INHERITED_ACE_ 0x10

/* ------------------------------------------------------------------- */
static void bluetooth(void)
{
    BT_FIND_RADIO_PARAMS rp = { sizeof(rp) }, bad = { 99 };
    HANDLE radio = (HANDLE)1;
    SetLastError(0);
    HANDLE f = BluetoothFindFirstRadio(&rp, &radio);
    check("BluetoothFindFirstRadio: no radio (ERROR_NO_MORE_ITEMS)", !f && !radio && GetLastError() == ERROR_NO_MORE_ITEMS);
    f = BluetoothFindFirstRadio(&bad, &radio);
    check("BluetoothFindFirstRadio: a wrong dwSize is ERROR_REVISION_MISMATCH", !f && GetLastError() == 1306);
    BYTE search[64] = { 40 }, info[600] = { 0 };
    f = BluetoothFindFirstDevice(search, info);
    check("BluetoothFindFirstDevice: no device", !f && GetLastError() == ERROR_NO_MORE_ITEMS);
    check("BluetoothIsConnectable: no", !BluetoothIsConnectable(0));

    /* A serial-port service record: ServiceClassIDList (1) { UUID16 0x1101 },
     * ProtocolDescriptorList (4) { { L2CAP 0x0100 } { RFCOMM 0x0003, channel 5 } },
     * ServiceName (0x100) "COM", and a 128-bit UUID (0x200) */
    static BYTE rec[] = {
        0x35, 46,
        0x09, 0x00, 0x01, 0x35, 0x03, 0x19, 0x11, 0x01,
        0x09, 0x00, 0x04, 0x35, 0x0C, 0x35, 0x03, 0x19, 0x01, 0x00, 0x35, 0x05, 0x19, 0x00, 0x03, 0x08, 0x05,
        0x09, 0x01, 0x00, 0x25, 0x03, 'C', 'O', 'M',
        0x09, 0x02, 0x00, 0x1C, 0x00, 0x00, 0x11, 0x01, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80,
        0x5F, 0x9B, 0x34, 0xFB,
    };
    rec[1] = (BYTE)(sizeof(rec) - 2);
    SDP_ELEMENT_DATA d;
    DWORD e = BluetoothSdpGetAttributeValue(rec, sizeof(rec), 0x100, &d);
    check("SDP: ServiceName is the string \"COM\"",
          !e && d.type == 4 && d.data.string.length == 3 && !memcmp(d.data.string.value, "COM", 3));
    e = BluetoothSdpGetAttributeValue(rec, sizeof(rec), 0x200, &d);
    check("SDP: a 128-bit UUID reads as the GUID 00001101-0000-1000-8000-00805F9B34FB",
          !e && d.type == 3 && d.specificType == 0x430 && d.data.uuid128.Data1 == 0x1101 && d.data.uuid128.Data3 == 0x1000 &&
          d.data.uuid128.Data4[0] == 0x80 && d.data.uuid128.Data4[7] == 0xFB);
    check("SDP: an attribute the record lacks is ERROR_FILE_NOT_FOUND",
          BluetoothSdpGetAttributeValue(rec, sizeof(rec), 0x300, &d) == ERROR_FILE_NOT_FOUND);
    check("SDP: a cut-off record is ERROR_INVALID_PARAMETER",
          BluetoothSdpGetAttributeValue(rec, 20, 0x100, &d) == ERROR_INVALID_PARAMETER);
    /* the RFCOMM channel, as Chromium walks the protocol list */
    int channel = -1, items = 0;
    e = BluetoothSdpGetAttributeValue(rec, sizeof(rec), 4, &d);
    if (!e && d.type == 6) {
        HANDLE it = 0;
        SDP_ELEMENT_DATA p;
        while (!BluetoothSdpGetContainerElementData(d.data.sequence.value, d.data.sequence.length, &it, &p)) {
            items++;
            HANDLE it2 = 0;
            SDP_ELEMENT_DATA u, ch;
            if (p.type == 6 &&
                !BluetoothSdpGetContainerElementData(p.data.sequence.value, p.data.sequence.length, &it2, &u) &&
                u.type == 3 && u.specificType == 0x130 && u.data.uuid16 == 3 &&
                !BluetoothSdpGetContainerElementData(p.data.sequence.value, p.data.sequence.length, &it2, &ch) &&
                ch.type == 1 && ch.specificType == 0x10)
                channel = ch.data.uint8;
        }
    }
    check("SDP: the protocol list has two entries and RFCOMM channel 5", items == 2 && channel == 5);
}

static void devices(void)
{
    static const GUID dev = { 0x189f2b77, 0x2ed6, 0x4b2c, { 1, 2, 3, 4, 5, 6, 7, 8 } };
    void *p = (void *)1;
    HRESULT hr = D3D12CreateDevice(0, 0xb000, &dev, &p);
    check("D3D12CreateDevice: DXGI_ERROR_UNSUPPORTED, no device", hr == (HRESULT)0x887A0004L && !p);
    hr = D3D12CreateDevice(0, 0xb000, &dev, 0);
    check("D3D12CreateDevice (support check): DXGI_ERROR_UNSUPPORTED", hr == (HRESULT)0x887A0004L);
    p = (void *)1;
    hr = D3D12GetDebugInterface(&dev, &p);
    check("D3D12GetDebugInterface: DXGI_ERROR_SDK_COMPONENT_MISSING", hr == (HRESULT)0x887A002DL && !p);
    HMODULE m = GetModuleHandleA("d3d12.dll");
    check("d3d12.dll: ordinals 101 and 102 are D3D12CreateDevice and D3D12GetDebugInterface",
          m && GetProcAddress(m, (LPCSTR)101) == GetProcAddress(m, "D3D12CreateDevice") &&
          GetProcAddress(m, (LPCSTR)102) == GetProcAddress(m, "D3D12GetDebugInterface") && GetProcAddress(m, (LPCSTR)101));

    PVOID iface = (PVOID)1;
    BOOL ok = WinUsb_Initialize(INVALID_HANDLE_VALUE, &iface);
    check("WinUsb_Initialize: not a WinUSB device (ERROR_INVALID_HANDLE)", !ok && !iface && GetLastError() == ERROR_INVALID_HANDLE);
    USHORT n = 5;
    BYTE caps[64];
    check("HidP_GetValueCaps: HIDP_STATUS_INVALID_PREPARSED_DATA",
          HidP_GetValueCaps(0, caps, &n, caps) == (LONG)0xC0110001 && n == 0);
    HANDLE set = SetupDiCreateDeviceInfoList(0, 0);
    BYTE info[32] = { 0 };
    ok = SetupDiOpenDeviceInfoW(set, L"USB\\VID_046D&PID_C52B\\5&1", 0, 0, info);
    check("SetupDiOpenDeviceInfoW: ERROR_NO_SUCH_DEVINST", !ok && GetLastError() == 0xE000020B);
    SetupDiDestroyDeviceInfoList(set);
}

static void environment(void)
{
    LPWSTR old = GetEnvironmentStringsW();
    SIZE_T len = 0;
    while (old[len] || old[len + 1]) len++;
    WCHAR *copy = malloc((len + 2) * sizeof(WCHAR));
    memcpy(copy, old, (len + 2) * sizeof(WCHAR));
    FreeEnvironmentStringsW(old);

    WCHAR block[] = L"QTW_A=1\0=C:=C:\\Tests\0QTW_B=two words\0";
    BOOL ok = SetEnvironmentStringsW(block);
    WCHAR v[64];
    check("SetEnvironmentStringsW replaces the environment",
          ok && GetEnvironmentVariableW(L"QTW_A", v, 64) == 1 && v[0] == '1' &&
          GetEnvironmentVariableW(L"QTW_B", v, 64) == 9 && GetEnvironmentVariableW(L"=C:", v, 64) == 8 &&
          !GetEnvironmentVariableW(L"PATH", v, 64) && GetLastError() == ERROR_ENVVAR_NOT_FOUND);
    WCHAR badb[] = L"QTW_A=3\0NOEQUALS\0";
    ok = SetEnvironmentStringsW(badb);
    check("SetEnvironmentStringsW: a string without '=' changes nothing (ERROR_INVALID_PARAMETER)",
          !ok && GetLastError() == ERROR_INVALID_PARAMETER && GetEnvironmentVariableW(L"QTW_A", v, 64) == 1 && v[0] == '1');
    ok = SetEnvironmentStringsW(copy);
    check("SetEnvironmentStringsW puts the old environment back",
          ok && GetEnvironmentVariableW(L"PATH", v, 64) > 0 && !GetEnvironmentVariableW(L"QTW_A", v, 64));
    free(copy);
}

/* -------- TreeResetNamedSecurityInfoW -------- */
static int progress_calls;
static void WINAPI progress(LPWSTR name, DWORD status, DWORD *invoke, PVOID args, BOOL set)
{
    (void)name; (void)invoke;
    if (args == (PVOID)&progress_calls && !status && set) progress_calls++;
}

static int all_inherited(LPWSTR path, int *count)
{
    PACL dacl = 0;
    PSECURITY_DESCRIPTOR sd = 0;
    SECURITY_DESCRIPTOR_CONTROL c = 0;
    DWORD rev;
    *count = -1;
    if (GetNamedSecurityInfoW(path, 1, DACL_SECURITY_INFORMATION, 0, 0, &dacl, 0, &sd) || !dacl) return 0;
    GetSecurityDescriptorControl(sd, &c, &rev);
    int ok = !(c & SE_DACL_PROTECTED);
    *count = dacl->AceCount;
    for (WORD i = 0; i < dacl->AceCount; i++) {
        ACE_HEADER *h;
        if (GetAce(dacl, i, (void **)&h) && !(h->AceFlags & INHERITED_ACE_)) ok = 0;
    }
    LocalFree(sd);
    return ok;
}

static void tree_reset(void)
{
    CreateDirectoryW(L"C:\\Tests", 0);
    CreateDirectoryW(L"C:\\Tests\\qtwtree", 0);
    CreateDirectoryW(L"C:\\Tests\\qtwtree\\sub", 0);
    HANDLE f = CreateFileW(L"C:\\Tests\\qtwtree\\sub\\file.txt", GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    /* the file first gets a protected DACL of its own */
    PSID users = 0, everyone = 0;
    SID_IDENTIFIER_AUTHORITY nt = { { 0, 0, 0, 0, 0, 5 } }, world = { { 0, 0, 0, 0, 0, 1 } };
    AllocateAndInitializeSid(&nt, 2, 32, 545, 0, 0, 0, 0, 0, 0, &users);
    AllocateAndInitializeSid(&world, 1, 0, 0, 0, 0, 0, 0, 0, 0, &everyone);
    BYTE own[128], top[128];
    PACL a = (PACL)own, b = (PACL)top;
    InitializeAcl(a, sizeof(own), ACL_REVISION);
    AddAccessAllowedAce(a, ACL_REVISION, GENERIC_READ, users);
    InitializeAcl(b, sizeof(top), ACL_REVISION);
    AddAccessAllowedAceEx(b, ACL_REVISION, OBJECT_INHERIT_ACE_ | CONTAINER_INHERIT_ACE_, GENERIC_ALL, everyone);
    WCHAR file[] = L"C:\\Tests\\qtwtree\\sub\\file.txt", root[] = L"C:\\Tests\\qtwtree", sub[] = L"C:\\Tests\\qtwtree\\sub";
    DWORD e = SetNamedSecurityInfoW(file, 1, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, 0, 0, a, 0);
    int n;
    check("a file with a protected DACL of its own", !e && !all_inherited(file, &n));
    progress_calls = 0;
    e = TreeResetNamedSecurityInfoW(root, 1, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, 0, 0, b, 0,
                                    FALSE, progress, 2 /* ProgressInvokeEveryObject */, &progress_calls);
    int nf, ns;
    int okf = all_inherited(file, &nf), oks = all_inherited(sub, &ns);
    check("TreeResetNamedSecurityInfoW: everything below inherits only the new entry",
          !e && okf && oks && nf == 1 && ns == 1);
    check("TreeResetNamedSecurityInfoW: the progress callback heard of the folder and both objects below", progress_calls == 3);
    DeleteFileW(file);
    RemoveDirectoryW(sub);
    RemoveDirectoryW(root);
    FreeSid(users);
    FreeSid(everyone);
}

static void network(void)
{
    LPBYTE buf = (LPBYTE)1;
    DWORD read = 9, total = 9, resume = 0;
    DWORD e = NetShareEnum(0, 1, &buf, (DWORD)-1, &read, &total, &resume);
    check("NetShareEnum: no shares on this machine", !e && !buf && !read && !total);
    WCHAR other[] = L"\\\\elsewhere";
    e = NetShareEnum(other, 1, &buf, (DWORD)-1, &read, &total, &resume);
    check("NetShareEnum: another machine is ERROR_BAD_NETPATH", e == 53);

    ULONG64 luid = 0, luid1 = 0, back = 0;
    GUID g;
    e = ConvertInterfaceNameToLuidW(L"ethernet_1", &luid);
    ConvertInterfaceIndexToLuid(1, &luid1);
    check("ConvertInterfaceNameToLuidW(\"ethernet_1\") is interface 1", !e && luid && luid == luid1);
    check("ConvertInterfaceNameToLuidW: another name is ERROR_INVALID_NAME",
          ConvertInterfaceNameToLuidW(L"wifi_9", &back) == ERROR_INVALID_NAME);
    e = ConvertInterfaceLuidToGuid(&luid, &g);
    DWORD e2 = ConvertInterfaceGuidToLuid(&g, &back);
    check("the interface's GUID converts back to its LUID", !e && !e2 && back == luid);
    ULONG size = 0;
    check("GetInterfaceInfo: no DHCP adapters (ERROR_NO_DATA)", GetInterfaceInfo(0, &size) == ERROR_NO_DATA);
}

static volatile LONG dns_done;
static volatile LONG dns_status;
static void WINAPI dns_complete(PVOID ctx, DNS_QUERY_RESULT *r)
{
    if (ctx == (PVOID)&dns_done) { dns_status = r->QueryStatus; InterlockedExchange(&dns_done, 1); }
}

static void dns(void)
{
    DNS_QUERY_REQUEST q = { 1, L"example.invalid", 1 /* A */, 0 };
    DNS_QUERY_RESULT r = { 1 };
    LONG s = DnsQueryEx(&q, &r, 0);
    check("DnsQueryEx: answers as DnsQuery_W does (DNS_ERROR_RCODE_NAME_ERROR)", s == 9003 && r.QueryStatus == 9003);
    q.pQueryCompletionCallback = dns_complete;
    q.pQueryContext = (PVOID)&dns_done;
    DNS_QUERY_RESULT r2 = { 1 };
    s = DnsQueryEx(&q, &r2, 0);
    for (int i = 0; i < 200 && !dns_done; i++) Sleep(10);
    check("DnsQueryEx with a completion routine: DNS_REQUEST_PENDING, then the routine",
          s == 9506 && dns_done && dns_status == 9003);
}

/* -------- WSAAccept -------- */
static int cond_calls;
static USHORT cond_port;
static int WINAPI condition(LPWSABUF caller, LPWSABUF cdata, void *qos, void *gqos, LPWSABUF callee, LPWSABUF edata,
                            unsigned int *g, DWORD_PTR cb)
{
    (void)cdata; (void)qos; (void)gqos; (void)edata; (void)g;
    cond_calls++;
    if (caller && caller->len >= sizeof(struct sockaddr_in)) cond_port = ((struct sockaddr_in *)caller->buf)->sin_port;
    if (!callee || callee->len < sizeof(struct sockaddr_in)) return 1;
    return (int)cb;                                            /* CF_ACCEPT 0, CF_REJECT 1 */
}

static void wsaaccept(void)
{
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    struct sockaddr_in a;
    int len = sizeof(a);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    SOCKET l = socket(AF_INET, SOCK_STREAM, 0);
    int ok = l != INVALID_SOCKET && !bind(l, (struct sockaddr *)&a, len) && !listen(l, 2) &&
             !getsockname(l, (struct sockaddr *)&a, &len);
    SOCKET c1 = socket(AF_INET, SOCK_STREAM, 0), c2 = socket(AF_INET, SOCK_STREAM, 0);
    ok = ok && !connect(c1, (struct sockaddr *)&a, len) && !connect(c2, (struct sockaddr *)&a, len);
    struct sockaddr_in me1, me2, peer;
    int l1 = sizeof(me1), l2 = sizeof(me2), lp = sizeof(peer);
    getsockname(c1, (struct sockaddr *)&me1, &l1);
    getsockname(c2, (struct sockaddr *)&me2, &l2);
    SOCKET s = ok ? WSAAccept(l, 0, 0, condition, 1) : INVALID_SOCKET;
    check("WSAAccept: the condition's CF_REJECT refuses the caller (WSAECONNREFUSED)",
          ok && s == INVALID_SOCKET && WSAGetLastError() == WSAECONNREFUSED && cond_calls == 1 && cond_port == me1.sin_port);
    s = ok ? WSAAccept(l, (struct sockaddr *)&peer, &lp, condition, 0) : INVALID_SOCKET;
    check("WSAAccept: CF_ACCEPT returns the connection and its address",
          s != INVALID_SOCKET && cond_calls == 2 && cond_port == me2.sin_port && lp == sizeof(peer) && peer.sin_port == me2.sin_port);
    char ch = 'x', got = 0;
    if (s != INVALID_SOCKET) { send(c2, &ch, 1, 0); recv(s, &got, 1, 0); }
    check("WSAAccept: the accepted connection carries data", got == 'x');
    closesocket(c1); closesocket(c2); closesocket(l);
}

/* -------- SetArcDirection -------- */
/* anything drawn within a pixel of (x, y) */
static int inked(const DWORD *px, int x, int y)
{
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            if (y + dy >= 0 && y + dy < 64 && (px[(y + dy) * 64 + x + dx] & 0xFFFFFF) != 0xFFFFFF) return 1;
    return 0;
}
static void arcs(void)
{
    HDC dc = CreateCompatibleDC(0);
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), 64, -64, 1, 32, BI_RGB } };
    void *bits = 0;
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, 0, 0);
    SelectObject(dc, bm);
    check("GetArcDirection: counterclockwise by default", GetArcDirection(dc) == 1);
    check("SetArcDirection(AD_CLOCKWISE) returns the old direction", SetArcDirection(dc, 2) == 1 && GetArcDirection(dc) == 2);
    check("SetArcDirection: 3 is ERROR_INVALID_PARAMETER", !SetArcDirection(dc, 3) && GetLastError() == ERROR_INVALID_PARAMETER);
    /* from the right (60, 32) to the top (32, 4): counterclockwise is the
     * upper-right quarter, clockwise the other three quarters */
    DWORD *px = bits;
    SetArcDirection(dc, 1);
    memset(bits, 0xFF, 64 * 64 * 4);
    Arc(dc, 4, 4, 61, 61, 60, 32, 32, 4);
    int ccw_bottom = inked(px, 32, 61), ccw_ur = inked(px, 52, 12);
    SetArcDirection(dc, 2);
    memset(bits, 0xFF, 64 * 64 * 4);
    Arc(dc, 4, 4, 61, 61, 60, 32, 32, 4);
    int cw_bottom = inked(px, 32, 61), cw_ur = inked(px, 52, 12);
    check("Arc: counterclockwise draws the upper right, not the bottom", ccw_ur && !ccw_bottom);
    check("Arc: clockwise draws through the bottom, not the upper right", cw_bottom && !cw_ur);
    check("CancelDC: TRUE (drawing is synchronous)", CancelDC(dc));
    DeleteDC(dc);
    DeleteObject(bm);
}

static void appcontainer(void)
{
    PSID sid = 0, derived = 0;
    DeleteAppContainerProfile(L"NovaOS.QtWebTest");
    HRESULT hr = CreateAppContainerProfile(L"NovaOS.QtWebTest", L"QtWeb test", L"qtwebtest's container", 0, 0, &sid);
    DeriveAppContainerSidFromAppContainerName(L"NovaOS.QtWebTest", &derived);
    check("CreateAppContainerProfile: S_OK and the container's SID", hr == S_OK && sid && derived && EqualSid(sid, derived));
    PSID again = 0;
    hr = CreateAppContainerProfile(L"NovaOS.QtWebTest", L"QtWeb test", L"qtwebtest's container", 0, 0, &again);
    check("CreateAppContainerProfile again: HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)", hr == (HRESULT)0x800700B7L && !again);
    WCHAR dir[MAX_PATH];
    GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    wcscat(dir, L"\\Packages\\NovaOS.QtWebTest");
    DWORD at = GetFileAttributesW(dir);
    check("CreateAppContainerProfile: the folder %LOCALAPPDATA%\\Packages\\NAME", at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY));
    check("CreateAppContainerProfile: a bad name is E_INVALIDARG",
          CreateAppContainerProfile(L"no spaces allowed", L"x", L"x", 0, 0, &again) == E_INVALIDARG);
    hr = DeleteAppContainerProfile(L"NovaOS.QtWebTest");
    check("DeleteAppContainerProfile removes it", hr == S_OK && GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES);
    if (sid) FreeSid(sid);
    if (derived) FreeSid(derived);
}

static void certificates(void)
{
    HANDLE root = CertOpenSystemStoreW(0, L"ROOT");
    const CCTX *a = 0, *b = 0, *c = 0;
    while ((c = CertEnumCertificatesInStore(root, c)) != 0) {
        /* two self-signed roots with different keys */
        if (!CertCompareCertificateName(1, &c->pCertInfo->Subject, &c->pCertInfo->Issuer)) continue;
        if (!a) {
            if (c->pCertInfo->SignatureAlgorithm.pszObjId && !strcmp(c->pCertInfo->SignatureAlgorithm.pszObjId, "1.2.840.113549.1.1.11"))
                a = CertDuplicateCertificateContext(c);          /* (sha256WithRSAEncryption) */
        }
        else if (c->pCertInfo->SubjectPublicKeyInfo.PublicKey.cbData != a->pCertInfo->SubjectPublicKeyInfo.PublicKey.cbData ||
                 memcmp(c->pCertInfo->SubjectPublicKeyInfo.PublicKey.pbData, a->pCertInfo->SubjectPublicKeyInfo.PublicKey.pbData,
                        a->pCertInfo->SubjectPublicKeyInfo.PublicKey.cbData)) {
            b = CertDuplicateCertificateContext(c);
            CertFreeCertificateContext(c);
            break;
        }
    }
    check("two self-signed roots in ROOT", a && b);
    if (a && b) {
        check("CertCompareCertificateName: a root's subject is its issuer, not another's",
              CertCompareCertificateName(1, &a->pCertInfo->Subject, &a->pCertInfo->Issuer) &&
              !CertCompareCertificateName(1, &a->pCertInfo->Subject, &b->pCertInfo->Subject));
        check("CryptVerifyCertificateSignatureEx: a root signed itself (issuer: certificate)",
              CryptVerifyCertificateSignatureEx(0, 1, 2, (void *)a, 2, (void *)a, 0, 0));
        CBLOB blob = { a->cbCertEncoded, a->pbCertEncoded };
        check("CryptVerifyCertificateSignatureEx: the same from its encoding and public key",
              CryptVerifyCertificateSignatureEx(0, 1, 1, &blob, 1, &a->pCertInfo->SubjectPublicKeyInfo, 0, 0));
        BOOL ok = CryptVerifyCertificateSignatureEx(0, 1, 2, (void *)a, 2, (void *)b, 0, 0);
        check("CryptVerifyCertificateSignatureEx: another root's key is NTE_BAD_SIGNATURE",
              !ok && GetLastError() == (DWORD)0x80090006L);
    }
    check("CertControlStore(CERT_STORE_CTRL_AUTO_RESYNC)", root && CertControlStore(root, 0, 4, 0));
    if (a) CertFreeCertificateContext(a);
    if (b) CertFreeCertificateContext(b);
    if (root) CertCloseStore(root, 0);
}

static volatile LONG proxy_status;
static volatile DWORD proxy_result, proxy_error;
static void WINAPI http_cb(HINTERNET h, DWORD_PTR ctx, DWORD status, LPVOID info, DWORD len)
{
    (void)h; (void)len;
    if (ctx != 0x5150 || status != WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) return;
    WINHTTP_ASYNC_RESULT *r = info;
    proxy_result = (DWORD)r->dwResult;
    proxy_error = r->dwError;
    InterlockedExchange(&proxy_status, 1);
}

typedef struct { BOOL fAutoDetect; DWORD dwFlags, dwAutoDetectFlags; LPCWSTR url; LPVOID r; DWORD r2; BOOL logon; } AUTOPROXY;

static void proxies(void)
{
    HINTERNET sync = WinHttpOpen(L"qtwebtest", 0, 0, 0, 0), res = 0;
    check("WinHttpCreateProxyResolver: a synchronous session is refused",
          WinHttpCreateProxyResolver(sync, &res) == ERROR_WINHTTP_INCORRECT_HANDLE_TYPE && !res);
    HINTERNET s = WinHttpOpen(L"qtwebtest", 0, 0, 0, WINHTTP_FLAG_ASYNC);
    WinHttpSetStatusCallback(s, http_cb, WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS, 0);
    DWORD e = WinHttpCreateProxyResolver(s, &res);
    check("WinHttpCreateProxyResolver on an asynchronous session", !e && res);
    AUTOPROXY o = { TRUE, 1 /* WINHTTP_AUTOPROXY_AUTO_DETECT */, 3 };
    e = WinHttpGetProxyForUrlEx(res, L"https://www.gog.com/", &o, 0x5150);
    for (int i = 0; i < 300 && !proxy_status; i++) Sleep(10);
    check("WinHttpGetProxyForUrlEx: pending, then ERROR_WINHTTP_AUTODETECTION_FAILED through the callback",
          e == ERROR_IO_PENDING && proxy_status && proxy_result == 6 && proxy_error == ERROR_WINHTTP_AUTODETECTION_FAILED);
    PROXY_RESULT pr = { 7, (void *)1 };
    e = WinHttpGetProxyResult(res, &pr);
    check("WinHttpGetProxyResult: no result (ERROR_WINHTTP_INCORRECT_HANDLE_STATE)",
          e == ERROR_WINHTTP_INCORRECT_HANDLE_STATE && !pr.cEntries && !pr.pEntries);
    WinHttpFreeProxyResult(&pr);
    WinHttpCloseHandle(res);
    WinHttpCloseHandle(s);
    WinHttpCloseHandle(sync);
}

typedef struct SecMgr { struct SecMgrVtbl *vt; } SecMgr;
struct SecMgrVtbl {
    HRESULT (WINAPI *QueryInterface)(SecMgr *, REFIID, void **);
    ULONG (WINAPI *AddRef)(SecMgr *);
    ULONG (WINAPI *Release)(SecMgr *);
    HRESULT (WINAPI *SetSecuritySite)(SecMgr *, void *);
    HRESULT (WINAPI *GetSecuritySite)(SecMgr *, void **);
    HRESULT (WINAPI *MapUrlToZone)(SecMgr *, LPCWSTR, DWORD *, DWORD);
    HRESULT (WINAPI *GetSecurityId)(SecMgr *, LPCWSTR, BYTE *, DWORD *, DWORD_PTR);
    HRESULT (WINAPI *ProcessUrlAction)(SecMgr *, LPCWSTR, DWORD, BYTE *, DWORD, BYTE *, DWORD, DWORD, DWORD);
};

static void zones(void)
{
    SecMgr *m = 0;
    HRESULT hr = CoInternetCreateSecurityManager(0, (void **)&m, 0);
    check("CoInternetCreateSecurityManager", hr == S_OK && m);
    if (!m) return;
    DWORD z1 = 9, z2 = 9, z3 = 9;
    m->vt->MapUrlToZone(m, L"https://www.gog.com/en/", &z1, 0);
    m->vt->MapUrlToZone(m, L"file:///C:/Programs/x.html", &z2, 0);
    m->vt->MapUrlToZone(m, L"C:\\Programs\\x.html", &z3, 0);
    check("MapUrlToZone: the web is the Internet zone, files the Local Machine zone", z1 == 3 && z2 == 0 && z3 == 0);
    BYTE id[128];
    DWORD n = sizeof(id);
    hr = m->vt->GetSecurityId(m, L"https://user@www.GOG.com:443/a", id, &n, 0);
    check("GetSecurityId: \"https:www.gog.com\" and zone 3",
          hr == S_OK && n == 21 && !memcmp(id, "https:www.gog.com", 17) && id[17] == 3 && !id[18]);
    DWORD p = 9;
    hr = m->vt->ProcessUrlAction(m, L"https://www.gog.com/", 0x1200 /* URLACTION_ACTIVEX_RUN */, (BYTE *)&p, sizeof(p), 0, 0, 0, 0);
    check("ProcessUrlAction: ActiveX from the Internet is refused (S_FALSE, URLPOLICY_DISALLOW)", hr == S_FALSE && p == 3);
    hr = m->vt->ProcessUrlAction(m, L"file:///C:/x.html", 0x1200, (BYTE *)&p, sizeof(p), 0, 0, 0, 0);
    check("ProcessUrlAction: the Local Machine zone allows it", hr == S_OK && p == 0);
    m->vt->Release(m);
}

static void crt(void)
{
    wchar_t b[16];
    check("_ultow_s(0xFFFFFFFF, 16)", !_ultow_s(0xFFFFFFFFul, b, 16, 16) && !wcscmp(b, L"ffffffff"));
    check("_ultow_s: too small a buffer is ERANGE", _ultow_s(123456ul, b, 4, 10) == ERANGE && !b[0]);
}

int main(void)
{
    bluetooth();
    devices();
    environment();
    tree_reset();
    network();
    dns();
    wsaaccept();
    arcs();
    appcontainer();
    certificates();
    proxies();
    zones();
    crt();
    printf("qtwebtest: %d passed, %d failed\n", passed, failed);
    return failed != 0;
}
