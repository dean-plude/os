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

/* IPv6 text (RFC 4291 2.2: groups, one "::", an IPv4 tail) */
static int pton6(const char *src, unsigned char out[16])
{
    unsigned short w[8];
    int n = 0, gap = -1;
    const char *p = src;
    if (p[0] == ':' && p[1] == ':') { gap = 0; p += 2; }
    else if (*p == ':') return 0;
    while (*p) {
        const char *start = p;
        unsigned v = 0;
        int digits = 0;
        while (digits < 5) {
            char c = *p;
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                  : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) break;
            v = v * 16 + (unsigned)d;
            digits++;
            p++;
        }
        if (*p == '.') {                                   /* the IPv4 tail */
            if (n > 6) return 0;
            unsigned char b[4];
            if (inet_pton(AF_INET, start, b) != 1) return 0;
            w[n++] = (unsigned short)(b[0] << 8 | b[1]);
            w[n++] = (unsigned short)(b[2] << 8 | b[3]);
            break;
        }
        if (!digits || digits > 4 || n >= 8) return 0;
        w[n++] = (unsigned short)v;
        if (!*p) break;
        if (*p != ':') return 0;
        p++;
        if (*p == ':') {
            if (gap >= 0) return 0;
            gap = n;
            p++;
            if (!*p) break;
        } else if (!*p) return 0;
    }
    if (gap < 0 ? n != 8 : n > 7) return 0;
    unsigned short full[8] = { 0 };
    if (gap < 0) for (int i = 0; i < 8; i++) full[i] = w[i];
    else {
        for (int i = 0; i < gap; i++) full[i] = w[i];
        for (int i = gap; i < n; i++) full[8 - (n - i)] = w[i];
    }
    for (int i = 0; i < 8; i++) { out[2 * i] = (unsigned char)(full[i] >> 8); out[2 * i + 1] = (unsigned char)full[i]; }
    return 1;
}

/* RFC 5952: lowercase, no leading zeros, the longest run of two or more
 * zero groups as "::", IPv4-mapped addresses with a dotted tail */
static int ntop6(const unsigned char *a, char *out)
{
    unsigned short w[8];
    for (int i = 0; i < 8; i++) w[i] = (unsigned short)(a[2 * i] << 8 | a[2 * i + 1]);
    int best = -1, blen = 0;
    for (int i = 0; i < 8;) {
        if (w[i]) { i++; continue; }
        int j = i;
        while (j < 8 && !w[j]) j++;
        if (j - i > blen && j - i >= 2) { best = i; blen = j - i; }
        i = j;
    }
    int mapped = !w[0] && !w[1] && !w[2] && !w[3] && !w[4] && w[5] == 0xFFFF;
    int n = 0;
    for (int i = 0; i < 8; i++) {
        if (i == best) { out[n++] = ':'; if (i == 0) out[n++] = ':'; i += blen - 1; continue; }
        if (mapped && i == 6) {
            char t[16];
            inet_ntop(AF_INET, a + 12, t, sizeof(t));
            for (int k = 0; t[k]; k++) out[n++] = t[k];
            break;
        }
        static const char hex[] = "0123456789abcdef";
        int started = 0;
        for (int sh = 12; sh >= 0; sh -= 4) {
            int d = (w[i] >> sh) & 0xF;
            if (d || started || sh == 0) { out[n++] = hex[d]; started = 1; }
        }
        if (i < 7) out[n++] = ':';
    }
    out[n] = 0;
    return n;
}

int inet_pton(int af, const char *src, void *dst)
{
    if (af == AF_INET6) return pton6(src, dst);
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
    if (af == AF_INET6) {
        char t[48];
        int n = ntop6(src, t);
        if ((size_t)n + 1 > size) { set_err(WSAEINVAL); return 0; }
        memcpy(dst, t, (size_t)n + 1);
        return dst;
    }
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
    char tmp[48];
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

/* -----------------------------------------------------------------------
 * WSAEventSelect: a helper thread looks at the registered sockets (with
 * select) and sets their events; WSAEnumNetworkEvents reports and clears
 * ----------------------------------------------------------------------- */
WINBASEAPI VOID WINAPI Sleep(DWORD ms);

typedef struct { SOCKET s; WSAEVENT ev; long mask, pending; int closed; } EvSel;
static EvSel g_evsel[64];
static volatile long g_evsel_lock, g_evsel_thread;

static void es_lock(void)   { while (__atomic_exchange_n(&g_evsel_lock, 1, __ATOMIC_ACQUIRE)) Sleep(0); }
static void es_unlock(void) { __atomic_store_n(&g_evsel_lock, 0, __ATOMIC_RELEASE); }

static DWORD WINAPI evsel_thread(void *arg)
{
    (void)arg;
    for (;;) {
        fd_set rd, wr;
        rd.fd_count = wr.fd_count = 0;
        es_lock();
        for (int i = 0; i < 64; i++) {
            if (!g_evsel[i].ev) continue;
            if (g_evsel[i].mask & (FD_READ | FD_ACCEPT | FD_CLOSE)) rd.fd_array[rd.fd_count++] = g_evsel[i].s;
            if (g_evsel[i].mask & (FD_WRITE | FD_CONNECT)) wr.fd_array[wr.fd_count++] = g_evsel[i].s;
        }
        es_unlock();
        if (!rd.fd_count && !wr.fd_count) { Sleep(20); continue; }
        struct timeval tv = { 0, 20000 };
        if (select(0, &rd, &wr, 0, &tv) <= 0) { Sleep(5); continue; }
        es_lock();
        for (int i = 0; i < 64; i++) {
            EvSel *e = &g_evsel[i];
            if (!e->ev) continue;
            long got = 0;
            if (__WSAFDIsSet(e->s, &rd)) {
                char c;
                int n = recv(e->s, &c, 1, MSG_PEEK);
                if (n == 0) { got |= FD_CLOSE; e->closed = 1; }
                else got |= (e->mask & FD_ACCEPT) ? FD_ACCEPT : FD_READ;
            }
            if (__WSAFDIsSet(e->s, &wr)) got |= (e->mask & FD_CONNECT) ? FD_CONNECT : FD_WRITE;
            got &= e->mask & ~e->pending;
            if (e->closed) got &= ~FD_READ;
            if (got) { e->pending |= got; SetEvent(e->ev); }
        }
        es_unlock();
        Sleep(5);
    }
}

/* Whether @s is a socket (a pipe or file handle is not: WSAENOTSOCK) */
static int is_socket(SOCKET s)
{
    BYTE st[3];
    return NtNovaSockCtl((INT_PTR)s, 4, 0, st) == 0;
}

int WSAEventSelect(SOCKET s, WSAEVENT ev, long events)
{
    if (!is_socket(s)) { set_err(WSAENOTSOCK); return SOCKET_ERROR; }
    es_lock();
    int free = -1, at = -1;
    for (int i = 0; i < 64; i++) {
        if (g_evsel[i].ev && g_evsel[i].s == s) at = i;
        if (!g_evsel[i].ev && free < 0) free = i;
    }
    if (at < 0) at = free;
    if (at < 0) { es_unlock(); set_err(WSAENOBUFS); return SOCKET_ERROR; }
    if (!ev || !events) memset(&g_evsel[at], 0, sizeof(EvSel));
    else { g_evsel[at].s = s; g_evsel[at].ev = ev; g_evsel[at].mask = events; g_evsel[at].pending = 0; g_evsel[at].closed = 0; }
    es_unlock();
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);                       /* as on Windows: the socket is non-blocking now */
    if (!__atomic_exchange_n(&g_evsel_thread, 1, __ATOMIC_ACQ_REL)) {
        HANDLE t = CreateThread(0, 64 * 1024, (LPTHREAD_START_ROUTINE)evsel_thread, 0, 0, 0);
        if (t) CloseHandle(t);
    }
    return 0;
}

int WSAEnumNetworkEvents(SOCKET s, WSAEVENT ev, LPWSANETWORKEVENTS out)
{
    if (!is_socket(s)) { set_err(WSAENOTSOCK); return SOCKET_ERROR; }   /* (gnulib's poll tells pipes this way) */
    memset(out, 0, sizeof(*out));
    es_lock();
    for (int i = 0; i < 64; i++) {
        if (!g_evsel[i].ev || g_evsel[i].s != s) continue;
        out->lNetworkEvents = g_evsel[i].pending;
        g_evsel[i].pending = 0;
        break;
    }
    es_unlock();
    if (ev) ResetEvent(ev);
    return 0;
}

/* getnameinfo: numeric (the names are the addresses; no reverse lookups) */
int getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host, DWORD hostlen, char *serv, DWORD servlen, int flags)
{
    (void)flags;
    if (!sa || (sa->sa_family != AF_INET && sa->sa_family != AF_INET6)) return WSAEAFNOSUPPORT;
    if (sa->sa_family == AF_INET && salen < (socklen_t)sizeof(struct sockaddr_in)) return WSAEFAULT;
    if (sa->sa_family == AF_INET6 && salen < (socklen_t)sizeof(struct sockaddr_in6)) return WSAEFAULT;
    const void *addr;
    u_short port;
    if (sa->sa_family == AF_INET) { addr = &((const struct sockaddr_in *)sa)->sin_addr; port = ((const struct sockaddr_in *)sa)->sin_port; }
    else { addr = (const BYTE *)sa + 8; port = *(const u_short *)((const BYTE *)sa + 2); }
    if (host && hostlen && !inet_ntop(sa->sa_family, addr, host, hostlen)) return WSAEFAULT;
    if (serv && servlen) {
        char t[8];
        unsigned p = ntohs(port), k = 0;
        char r[8];
        do { r[k++] = (char)('0' + p % 10); p /= 10; } while (p);
        for (unsigned i = 0; i < k; i++) t[i] = r[k - 1 - i];
        t[k] = 0;
        if (k + 1 > servlen) return WSAEFAULT;
        memcpy(serv, t, k + 1);
    }
    return 0;
}

/* "a.b.c.d[:port]", or "fd00::1[%zone]" / "[fd00::1%zone]:port" */
static void put_dec(char *t, size_t *n, unsigned v)
{
    char d[12]; int k = 0;
    do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) t[(*n)++] = d[--k];
    t[*n] = 0;
}

int WSAAddressToStringA(struct sockaddr *sa, DWORD len, void *info, char *out, DWORD *outlen)
{
    (void)info;
    char tmp[80];
    size_t n;
    if (!sa || !outlen) { set_err(WSAEINVAL); return SOCKET_ERROR; }
    if (sa->sa_family == AF_INET && len >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in *in = (struct sockaddr_in *)sa;
        inet_ntop(AF_INET, &in->sin_addr, tmp, 16);
        n = strlen(tmp);
        if (in->sin_port) { tmp[n++] = ':'; put_dec(tmp, &n, ntohs(in->sin_port)); }
    } else if (sa->sa_family == AF_INET6 && len >= sizeof(struct sockaddr_in6)) {
        struct sockaddr_in6 *in = (struct sockaddr_in6 *)sa;
        n = 0;
        if (in->sin6_port) tmp[n++] = '[';
        n += (size_t)ntop6(in->sin6_addr.s6_addr, tmp + n);
        if (in->sin6_scope_id) { tmp[n++] = '%'; put_dec(tmp, &n, in->sin6_scope_id); }
        if (in->sin6_port) { tmp[n++] = ']'; tmp[n++] = ':'; put_dec(tmp, &n, ntohs(in->sin6_port)); }
        tmp[n] = 0;
    } else {
        set_err(WSAEINVAL);
        return SOCKET_ERROR;
    }
    DWORD need = (DWORD)strlen(tmp) + 1;
    if (!out || *outlen < need) { *outlen = need; set_err(WSAEFAULT); return SOCKET_ERROR; }
    memcpy(out, tmp, need);
    *outlen = need;
    return 0;
}
int WSAAddressToStringW(struct sockaddr *sa, DWORD len, void *info, WCHAR *out, DWORD *outlen)
{
    char tmp[80];
    DWORD n = sizeof(tmp);
    if (!outlen || WSAAddressToStringA(sa, len, info, tmp, &n)) return SOCKET_ERROR;
    if (!out || *outlen < n) { *outlen = n; set_err(WSAEFAULT); return SOCKET_ERROR; }
    for (DWORD i = 0; i < n; i++) out[i] = (WCHAR)tmp[i];
    *outlen = n;
    return 0;
}

int GetNameInfoW(const struct sockaddr *sa, socklen_t salen, WCHAR *host, DWORD hostlen, WCHAR *serv, DWORD servlen, int flags)
{
    char h[64], s[16];
    int r = getnameinfo(sa, salen, host ? h : 0, host ? sizeof(h) : 0, serv ? s : 0, serv ? sizeof(s) : 0, flags);
    if (r) return r;
    if (host) { DWORD i = 0; for (; h[i] && i < hostlen - 1; i++) host[i] = (WCHAR)h[i]; host[i] = 0; }
    if (serv) { DWORD i = 0; for (; s[i] && i < servlen - 1; i++) serv[i] = (WCHAR)s[i]; serv[i] = 0; }
    return 0;
}

/* Sharing a socket with another process is not supported */
int WSADuplicateSocketW(SOCKET s, DWORD pid, void *info)
{
    (void)s; (void)pid; (void)info;
    set_err(WSAEINVAL);
    return SOCKET_ERROR;
}
int WSADuplicateSocketA(SOCKET s, DWORD pid, void *info) { return WSADuplicateSocketW(s, pid, info); }

/* WSAConnect: caller and callee data and QoS are not supported (ignored) */
int WSAConnect(SOCKET s, const struct sockaddr *to, int len, void *caller, void *callee, void *sqos, void *gqos)
{
    (void)caller; (void)callee; (void)sqos; (void)gqos;
    return connect(s, to, len);
}

/* "a.b.c.d[:port]" into a sockaddr_in; "fd00::1[%zone]" or
 * "[fd00::1[%zone]]:port" into a sockaddr_in6 */
int WSAStringToAddressA(char *str, int family, void *info, struct sockaddr *sa, int *len)
{
    (void)info;
    if (!str || !sa || !len || (family != AF_INET && family != AF_INET6)) { set_err(WSAEINVAL); return SOCKET_ERROR; }
    int need = family == AF_INET6 ? (int)sizeof(struct sockaddr_in6) : (int)sizeof(struct sockaddr_in);
    if (*len < need) { *len = need; set_err(WSAEFAULT); return SOCKET_ERROR; }
    char host[64];
    int i = 0, j = 0;
    unsigned port = 0, zone = 0;
    if (family == AF_INET6) {
        int br = str[0] == '[';
        for (i = br; str[i] && str[i] != ']' && str[i] != '%' && j < 63; i++) host[j++] = str[i];
        host[j] = 0;
        if (str[i] == '%') for (i++; str[i] >= '0' && str[i] <= '9'; i++) zone = zone * 10 + (unsigned)(str[i] - '0');
        if (br) {
            if (str[i] != ']') { set_err(WSAEINVAL); return SOCKET_ERROR; }
            i++;
            if (str[i] == ':') for (i++; str[i] >= '0' && str[i] <= '9'; i++) port = port * 10 + (unsigned)(str[i] - '0');
        }
        struct sockaddr_in6 *in = (struct sockaddr_in6 *)sa;
        memset(in, 0, sizeof(*in));
        in->sin6_family = AF_INET6;
        in->sin6_port = htons((u_short)port);
        in->sin6_scope_id = zone;
        if (pton6(host, in->sin6_addr.s6_addr) != 1) { set_err(WSAEINVAL); return SOCKET_ERROR; }
        *len = need;
        return 0;
    }
    for (; str[i] && str[i] != ':' && i < 31; i++) host[i] = str[i];
    host[i] = 0;
    if (str[i] == ':') for (const char *p = str + i + 1; *p >= '0' && *p <= '9'; p++) port = port * 10 + (unsigned)(*p - '0');
    struct sockaddr_in *in = (struct sockaddr_in *)sa;
    memset(in, 0, sizeof(*in));
    in->sin_family = AF_INET;
    in->sin_port = htons((u_short)port);
    if (inet_pton(AF_INET, host, &in->sin_addr) != 1) { set_err(WSAEINVAL); return SOCKET_ERROR; }
    *len = need;
    return 0;
}
int WSAStringToAddressW(WCHAR *str, int family, void *info, struct sockaddr *sa, int *len)
{
    char a[64];
    int i = 0;
    for (; str && str[i] && i < 63; i++) a[i] = (char)str[i];
    a[i] = 0;
    return WSAStringToAddressA(str ? a : 0, family, info, sa, len);
}
