/*
 * wsa.c — the Winsock 2 extensions: WSASocket, WSASend/WSARecv (and the
 * To/From forms), events, WSAIoctl, the wide name functions, inet_pton.
 *
 * NovaOS sockets are synchronous underneath, so an overlapped operation
 * runs to completion inside the call: its OVERLAPPED is filled in, its
 * event set and, when the socket is bound to an I/O completion port, a
 * completion packet posted (by kernel32, as for files); a completion
 * routine is called before the function returns.
 */
#define WS2_EXPORT
#define NOVA_BUILD_KERNEL32
#include <winsock2.h>
#include <winternl.h>

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
size_t strlen(const char *s);
WINBASEAPI LPVOID WINAPI HeapAlloc(HANDLE, DWORD, SIZE_T);
WINBASEAPI HANDLE WINAPI GetProcessHeap(void);
WINBASEAPI BOOL WINAPI HeapFree(HANDLE, DWORD, LPVOID);
WINBASEAPI BOOL WINAPI SetEvent(HANDLE);
WINBASEAPI BOOL WINAPI ResetEvent(HANDLE);
__declspec(dllimport) void WINAPI NovaIoComplete(HANDLE h, OVERLAPPED *o, LONG status, DWORD bytes);

static void set_err(int e) { *(DWORD *)(NtCurrentTebBytes() + TEB_LAST_ERROR) = (DWORD)e; }

int __WSAFDIsSet(SOCKET fd, fd_set *set)
{
    for (UINT i = 0; i < set->fd_count; i++) if (set->fd_array[i] == fd) return 1;
    return 0;
}

SOCKET WSASocketW(int af, int type, int protocol, LPWSAPROTOCOL_INFOW info, GROUP g, DWORD flags)
{
    (void)info; (void)g; (void)flags;
    return socket(af, type, protocol);
}

SOCKET WSASocketA(int af, int type, int protocol, LPWSAPROTOCOL_INFOA info, GROUP g, DWORD flags)
{
    (void)info; (void)g; (void)flags;
    return socket(af, type, protocol);
}

/* finish an operation that completed at once */
static int complete(SOCKET s, LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr, int err, DWORD bytes)
{
    if (!ov) return err ? SOCKET_ERROR : 0;
    if (err) return SOCKET_ERROR;                /* failed at once: no completion is reported */
    NovaIoComplete((HANDLE)s, ov, 0, bytes);
    if (cr) cr(0, bytes, ov, 0);
    return 0;
}

int WSASend(SOCKET s, LPWSABUF bufs, DWORD n, LPDWORD sent, DWORD flags, LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    DWORD total = 0;
    for (DWORD i = 0; i < n; i++) {
        ULONG off = 0;
        while (off < bufs[i].len) {
            int r = send(s, bufs[i].buf + off, (int)(bufs[i].len - off), (int)flags);
            if (r == SOCKET_ERROR) {
                if (!total) return complete(s, ov, cr, 1, 0);
                goto done;                        /* report what went out */
            }
            off += (ULONG)r;
            total += (DWORD)r;
        }
    }
done:
    if (sent) *sent = total;
    return complete(s, ov, cr, 0, total);
}

int WSARecv(SOCKET s, LPWSABUF bufs, DWORD n, LPDWORD got, LPDWORD flags, LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    DWORD total = 0;
    int f = flags ? (int)*flags : 0;
    for (DWORD i = 0; i < n; i++) {
        if (!bufs[i].len) continue;
        int r = recv(s, bufs[i].buf, (int)bufs[i].len, f);
        if (r == SOCKET_ERROR) {
            if (!total) return complete(s, ov, cr, 1, 0);
            break;
        }
        total += (DWORD)r;
        if ((ULONG)r < bufs[i].len) break;        /* no more waiting data: don't block for the next buffer */
    }
    if (got) *got = total;
    if (flags) *flags = 0;
    return complete(s, ov, cr, 0, total);
}

int WSASendTo(SOCKET s, LPWSABUF bufs, DWORD n, LPDWORD sent, DWORD flags, const struct sockaddr *to, int tolen,
              LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    if (!to) return WSASend(s, bufs, n, sent, flags, ov, cr);
    /* one datagram: gather the buffers */
    DWORD len = 0;
    for (DWORD i = 0; i < n; i++) len += bufs[i].len;
    char *tmp = HeapAlloc(GetProcessHeap(), 0, len ? len : 1);
    if (!tmp) { set_err(WSAENOBUFS); return SOCKET_ERROR; }
    for (DWORD i = 0, o = 0; i < n; o += bufs[i].len, i++) memcpy(tmp + o, bufs[i].buf, bufs[i].len);
    int r = sendto(s, tmp, (int)len, (int)flags, to, tolen);
    HeapFree(GetProcessHeap(), 0, tmp);
    if (r == SOCKET_ERROR) return complete(s, ov, cr, 1, 0);
    if (sent) *sent = (DWORD)r;
    return complete(s, ov, cr, 0, (DWORD)r);
}

int WSARecvFrom(SOCKET s, LPWSABUF bufs, DWORD n, LPDWORD got, LPDWORD flags, struct sockaddr *from, int *fromlen,
                LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    if (!n) { set_err(WSAEINVAL); return SOCKET_ERROR; }
    int r = recvfrom(s, bufs[0].buf, (int)bufs[0].len, flags ? (int)*flags : 0, from, fromlen);
    if (r == SOCKET_ERROR) return complete(s, ov, cr, 1, 0);
    if (got) *got = (DWORD)r;
    if (flags) *flags = 0;
    return complete(s, ov, cr, 0, (DWORD)r);
}

BOOL WSAGetOverlappedResult(SOCKET s, LPWSAOVERLAPPED ov, LPDWORD bytes, BOOL wait, LPDWORD flags)
{
    (void)s;
    if (!ov) { set_err(WSAEFAULT); return FALSE; }
    if (ov->Internal == 0x103 /* STATUS_PENDING */) {
        if (!wait || !ov->hEvent) { set_err(WSA_IO_INCOMPLETE); return FALSE; }
        WaitForSingleObject((HANDLE)((ULONG_PTR)ov->hEvent & ~(ULONG_PTR)1), INFINITE);
    }
    if (bytes) *bytes = (DWORD)ov->InternalHigh;
    if (flags) *flags = 0;
    if (ov->Internal) { set_err(WSAECONNRESET); return FALSE; }
    return TRUE;
}

int WSAIoctl(SOCKET s, DWORD code, LPVOID in, DWORD inlen, LPVOID out, DWORD outlen, LPDWORD ret,
             LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    (void)inlen; (void)out; (void)outlen;
    if (ret) *ret = 0;
    switch (code) {
    case 0x8004667E: {                            /* FIONBIO */
        u_long v = in ? *(u_long *)in : 0;
        if (ioctlsocket(s, (long)code, &v)) return SOCKET_ERROR;
        return complete(s, ov, cr, 0, 0);
    }
    case SIO_KEEPALIVE_VALS:
        return complete(s, ov, cr, 0, 0);
    default:                                      /* AcceptEx, ConnectEx... and the rest: not available */
        set_err(WSAEOPNOTSUPP);
        return SOCKET_ERROR;
    }
}

/* ---- events: plain manual-reset kernel32 events ---- */
WSAEVENT WSACreateEvent(void) { return CreateEventW(0, TRUE, FALSE, 0); }
BOOL WSACloseEvent(WSAEVENT e) { return CloseHandle(e); }
BOOL WSASetEvent(WSAEVENT e) { return SetEvent(e); }
BOOL WSAResetEvent(WSAEVENT e) { return ResetEvent(e); }
DWORD WSAWaitForMultipleEvents(DWORD n, const WSAEVENT *events, BOOL all, DWORD ms, BOOL alertable)
{
    return WaitForMultipleObjectsEx(n, events, all, ms, alertable);
}

/* ---- names ---- */
static char *to_utf8(PCWSTR w)
{
    if (!w) return 0;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, 0, 0, 0, 0);
    char *s = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)(n > 0 ? n : 1));
    if (s) { s[0] = 0; WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, 0, 0); }
    return s;
}

static PWSTR to_wide(const char *s)
{
    if (!s) return 0;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, 0, 0);
    PWSTR w = HeapAlloc(GetProcessHeap(), 0, 2 * (SIZE_T)(n > 0 ? n : 1));
    if (w) { w[0] = 0; MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n); }
    return w;
}

int GetAddrInfoW(PCWSTR node, PCWSTR service, const ADDRINFOW *hints, PADDRINFOW *res)
{
    char *n = to_utf8(node), *sv = to_utf8(service);
    struct addrinfo h, *ai = 0;
    if (hints) {
        memset(&h, 0, sizeof(h));
        h.ai_flags = hints->ai_flags;
        h.ai_family = hints->ai_family;
        h.ai_socktype = hints->ai_socktype;
        h.ai_protocol = hints->ai_protocol;
    }
    int r = getaddrinfo(n, sv, hints ? &h : 0, &ai);
    HeapFree(GetProcessHeap(), 0, n);
    HeapFree(GetProcessHeap(), 0, sv);
    *res = 0;
    if (r) return r;
    PADDRINFOW *tail = res;
    for (struct addrinfo *a = ai; a; a = a->ai_next) {
        ADDRINFOW *w = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ADDRINFOW) + a->ai_addrlen);
        if (!w) break;
        w->ai_flags = a->ai_flags;
        w->ai_family = a->ai_family;
        w->ai_socktype = a->ai_socktype;
        w->ai_protocol = a->ai_protocol;
        w->ai_addrlen = a->ai_addrlen;
        w->ai_addr = (struct sockaddr *)(w + 1);
        memcpy(w->ai_addr, a->ai_addr, a->ai_addrlen);
        w->ai_canonname = to_wide(a->ai_canonname);
        *tail = w;
        tail = &w->ai_next;
    }
    freeaddrinfo(ai);
    return 0;
}

void FreeAddrInfoW(PADDRINFOW ai)
{
    while (ai) {
        PADDRINFOW next = ai->ai_next;
        if (ai->ai_canonname) HeapFree(GetProcessHeap(), 0, ai->ai_canonname);
        HeapFree(GetProcessHeap(), 0, ai);
        ai = next;
    }
}

int inet_pton(int af, const char *src, void *dst)
{
    if (af != AF_INET) { set_err(WSAEAFNOSUPPORT); return -1; }
    unsigned char b[4];
    int part = 0;
    for (const char *p = src;; p++) {
        if (*p < '0' || *p > '9') return 0;
        unsigned v = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (unsigned)(*p++ - '0'); if (++digits > 3) return 0; }
        if (v > 255 || part > 3) return 0;
        b[part++] = (unsigned char)v;
        if (!*p) break;
        if (*p != '.') return 0;
    }
    if (part != 4) return 0;
    memcpy(dst, b, 4);
    return 1;
}

const char *inet_ntop(int af, const void *src, char *dst, size_t size)
{
    if (af != AF_INET) { set_err(WSAEAFNOSUPPORT); return 0; }
    const unsigned char *b = src;
    char tmp[16];
    int n = 0;
    for (int i = 0; i < 4; i++) {
        unsigned v = b[i];
        if (v >= 100) tmp[n++] = (char)('0' + v / 100);
        if (v >= 10) tmp[n++] = (char)('0' + v / 10 % 10);
        tmp[n++] = (char)('0' + v % 10);
        if (i < 3) tmp[n++] = '.';
    }
    tmp[n] = 0;
    if ((size_t)n + 1 > size) { set_err(WSAEINVAL); return 0; }
    memcpy(dst, tmp, (size_t)n + 1);
    return dst;
}

int InetPtonW(int af, PCWSTR src, void *dst)
{
    char *s = to_utf8(src);
    int r = s ? inet_pton(af, s, dst) : 0;
    HeapFree(GetProcessHeap(), 0, s);
    return r;
}

PCWSTR InetNtopW(int af, const void *src, PWSTR dst, size_t size)
{
    char tmp[16];
    if (!inet_ntop(af, src, tmp, sizeof(tmp))) return 0;
    size_t n = strlen(tmp);
    if (n + 1 > size) { set_err(WSAEINVAL); return 0; }
    for (size_t i = 0; i <= n; i++) dst[i] = (WCHAR)tmp[i];
    return dst;
}

int gethostname(char *name, int len)
{
    char tmp[64];
    DWORD n = sizeof(tmp);
    if (!GetComputerNameA(tmp, &n)) { memcpy(tmp, "NOVA-PC", 8); n = 7; }
    if (!name || len <= (int)n) { set_err(WSAEFAULT); return SOCKET_ERROR; }
    memcpy(name, tmp, n + 1);
    return 0;
}

int GetHostNameW(PWSTR name, int len)
{
    char tmp[64];
    if (gethostname(tmp, sizeof(tmp))) return SOCKET_ERROR;
    int n = (int)strlen(tmp);
    if (!name || len <= n) { set_err(WSAEFAULT); return SOCKET_ERROR; }
    for (int i = 0; i <= n; i++) name[i] = (WCHAR)tmp[i];
    return 0;
}

struct servent *getservbyname(const char *name, const char *proto)
{
    (void)name; (void)proto;
    set_err(WSANO_DATA);
    return 0;
}

/* ---- protocols: TCP and UDP over IPv4 (WSAPROTOCOL_INFOW is 628 bytes, the A form 372) ---- */
static int enum_protocols(const int *which, BYTE *buf, LPDWORD len, BOOL wide)
{
    static const struct { int type, proto, maxmsg; DWORD flags; const char *name; } all[] = {
        { 1, 6, 0, 0x2 | 0x4 | 0x10 | 0x40 | 0x20000, "MSAFD Tcpip [TCP/IP]" },       /* guaranteed, ordered, graceful close, IFS handles */
        { 2, 17, 65527, 0x1 | 0x8 | 0x200 | 0x20000, "MSAFD Tcpip [UDP/IP]" },        /* connectionless, message oriented, broadcast */
    };
    const DWORD size = wide ? 628 : 372;
    int pick[2], n = 0;
    for (int i = 0; i < 2; i++) {
        BOOL want = !which;
        for (const int *w = which; w && *w; w++) if (*w == all[i].proto) want = TRUE;
        if (want) pick[n++] = i;
    }
    if (!len) { set_err(WSAEFAULT); return SOCKET_ERROR; }
    if (!buf || *len < n * size) { *len = n * size; set_err(WSAENOBUFS); return SOCKET_ERROR; }
    memset(buf, 0, n * size);
    for (int k = 0; k < n; k++) {
        BYTE *e = buf + k * size;
        const int i = pick[k];
        *(DWORD *)(e + 0) = all[i].flags;                    /* dwServiceFlags1 */
        *(DWORD *)(e + 16) = 0x8;                            /* dwProviderFlags: PFL_MATCHES_PROTOCOL_ZERO */
        *(DWORD *)(e + 20) = 0xE70F1AA0;                     /* ProviderId: the MSAFD Tcpip provider */
        *(DWORD *)(e + 36) = 1001 + (DWORD)i;                /* dwCatalogEntryId */
        *(int *)(e + 40) = 1;                                /* ProtocolChain.ChainLen: a base protocol */
        int *f = (int *)(e + 72);
        f[0] = 2;                                            /* iVersion */
        f[1] = AF_INET;                                      /* iAddressFamily */
        f[2] = 16;                                           /* iMaxSockAddr */
        f[3] = 16;                                           /* iMinSockAddr */
        f[4] = all[i].type;                                  /* iSocketType */
        f[5] = all[i].proto;                                 /* iProtocol */
        f[7] = 1;                                            /* iNetworkByteOrder: big endian */
        *(DWORD *)(e + 108) = (DWORD)all[i].maxmsg;          /* dwMessageSize */
        for (int c = 0; all[i].name[c]; c++) {
            if (wide) ((WCHAR *)(e + 116))[c] = (WCHAR)all[i].name[c];
            else e[116 + c] = (BYTE)all[i].name[c];
        }
    }
    return n;
}

int WSAEnumProtocolsW(int *protocols, LPWSAPROTOCOL_INFOW buf, LPDWORD len) { return enum_protocols(protocols, (BYTE *)buf, len, TRUE); }
int WSAEnumProtocolsA(int *protocols, LPWSAPROTOCOL_INFOA buf, LPDWORD len) { return enum_protocols(protocols, (BYTE *)buf, len, FALSE); }
