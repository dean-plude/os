/*
 * wsock32.dll — Winsock 1.1, the same calls as ws2_32.dll under Winsock
 * 1.1's export ordinals.
 *
 * Programs built against the old library import by ordinal, and three of
 * the ordinals differ from ws2_32's: here inet_addr is 10, inet_ntoa 11
 * and ioctlsocket 12, where ws2_32 has ioctlsocket at 10.  Firefox's NSPR
 * (nss3.dll) reaches ioctlsocket through wsock32 ordinal 12, so pointing
 * the name at ws2_32 made every socket it opened "fail" to go
 * non-blocking.  Each entry point here calls ws2_32's own.
 */
#define WS2_EXPORT
#include <winsock2.h>

/* ws2_32's functions, found by name the first time one is called */
enum {
    F_accept, F_bind, F_closesocket, F_connect, F_getpeername, F_getsockname, F_getsockopt,
    F_htonl, F_htons, F_inet_addr, F_inet_ntoa, F_ioctlsocket, F_listen, F_ntohl, F_ntohs,
    F_recv, F_recvfrom, F_select, F_send, F_sendto, F_setsockopt, F_shutdown, F_socket,
    F_gethostbyaddr, F_gethostbyname, F_getservbyname, F_getservbyport, F_gethostname,
    F_WSAGetLastError, F_WSASetLastError, F_WSAStartup, F_WSACleanup, F___WSAFDIsSet, F_WSARecvEx,
    F_COUNT
};
static const char *const names[F_COUNT] = {
    "accept", "bind", "closesocket", "connect", "getpeername", "getsockname", "getsockopt",
    "htonl", "htons", "inet_addr", "inet_ntoa", "ioctlsocket", "listen", "ntohl", "ntohs",
    "recv", "recvfrom", "select", "send", "sendto", "setsockopt", "shutdown", "socket",
    "gethostbyaddr", "gethostbyname", "getservbyname", "getservbyport", "gethostname",
    "WSAGetLastError", "WSASetLastError", "WSAStartup", "WSACleanup", "__WSAFDIsSet", "WSARecvEx",
};
static FARPROC fns[F_COUNT];
static HMODULE ws2;

static FARPROC fn(int i)
{
    if (fns[i]) return fns[i];
    if (!ws2) ws2 = LoadLibraryA("ws2_32.dll");
    fns[i] = ws2 ? GetProcAddress(ws2, names[i]) : 0;
    return fns[i];
}
#define CALL(ret, name, args, ...) return ((ret (WSAAPI *)args)fn(F_##name))(__VA_ARGS__)

SOCKET WSAAPI accept(SOCKET s, struct sockaddr *a, int *l) { CALL(SOCKET, accept, (SOCKET, struct sockaddr *, int *), s, a, l); }
int WSAAPI bind(SOCKET s, const struct sockaddr *a, int l) { CALL(int, bind, (SOCKET, const struct sockaddr *, int), s, a, l); }
int WSAAPI closesocket(SOCKET s) { CALL(int, closesocket, (SOCKET), s); }
int WSAAPI connect(SOCKET s, const struct sockaddr *a, int l) { CALL(int, connect, (SOCKET, const struct sockaddr *, int), s, a, l); }
int WSAAPI getpeername(SOCKET s, struct sockaddr *a, int *l) { CALL(int, getpeername, (SOCKET, struct sockaddr *, int *), s, a, l); }
int WSAAPI getsockname(SOCKET s, struct sockaddr *a, int *l) { CALL(int, getsockname, (SOCKET, struct sockaddr *, int *), s, a, l); }
int WSAAPI getsockopt(SOCKET s, int lv, int o, char *v, int *l) { CALL(int, getsockopt, (SOCKET, int, int, char *, int *), s, lv, o, v, l); }
u_long WSAAPI htonl(u_long v) { CALL(u_long, htonl, (u_long), v); }
u_short WSAAPI htons(u_short v) { CALL(u_short, htons, (u_short), v); }
unsigned long WSAAPI inet_addr(const char *cp) { CALL(unsigned long, inet_addr, (const char *), cp); }
char *WSAAPI inet_ntoa(struct in_addr in) { CALL(char *, inet_ntoa, (struct in_addr), in); }
int WSAAPI ioctlsocket(SOCKET s, long cmd, u_long *arg) { CALL(int, ioctlsocket, (SOCKET, long, u_long *), s, cmd, arg); }
int WSAAPI listen(SOCKET s, int n) { CALL(int, listen, (SOCKET, int), s, n); }
u_long WSAAPI ntohl(u_long v) { CALL(u_long, ntohl, (u_long), v); }
u_short WSAAPI ntohs(u_short v) { CALL(u_short, ntohs, (u_short), v); }
int WSAAPI recv(SOCKET s, char *b, int l, int f) { CALL(int, recv, (SOCKET, char *, int, int), s, b, l, f); }
int WSAAPI recvfrom(SOCKET s, char *b, int l, int f, struct sockaddr *a, int *al) { CALL(int, recvfrom, (SOCKET, char *, int, int, struct sockaddr *, int *), s, b, l, f, a, al); }
int WSAAPI select(int n, fd_set *r, fd_set *w, fd_set *e, const struct timeval *t) { CALL(int, select, (int, fd_set *, fd_set *, fd_set *, const struct timeval *), n, r, w, e, t); }
int WSAAPI send(SOCKET s, const char *b, int l, int f) { CALL(int, send, (SOCKET, const char *, int, int), s, b, l, f); }
int WSAAPI sendto(SOCKET s, const char *b, int l, int f, const struct sockaddr *a, int al) { CALL(int, sendto, (SOCKET, const char *, int, int, const struct sockaddr *, int), s, b, l, f, a, al); }
int WSAAPI setsockopt(SOCKET s, int lv, int o, const char *v, int l) { CALL(int, setsockopt, (SOCKET, int, int, const char *, int), s, lv, o, v, l); }
int WSAAPI shutdown(SOCKET s, int how) { CALL(int, shutdown, (SOCKET, int), s, how); }
SOCKET WSAAPI socket(int af, int type, int proto) { CALL(SOCKET, socket, (int, int, int), af, type, proto); }
struct hostent *WSAAPI gethostbyaddr(const char *a, int l, int t) { CALL(struct hostent *, gethostbyaddr, (const char *, int, int), a, l, t); }
struct hostent *WSAAPI gethostbyname(const char *n) { CALL(struct hostent *, gethostbyname, (const char *), n); }
struct servent *WSAAPI getservbyname(const char *n, const char *p) { CALL(struct servent *, getservbyname, (const char *, const char *), n, p); }
struct servent *WSAAPI getservbyport(int port, const char *p) { CALL(struct servent *, getservbyport, (int, const char *), port, p); }
int WSAAPI gethostname(char *n, int l) { CALL(int, gethostname, (char *, int), n, l); }
int WSAAPI WSAGetLastError(void) { CALL(int, WSAGetLastError, (void)); }
void WSAAPI WSASetLastError(int e) { ((void (WSAAPI *)(int))fn(F_WSASetLastError))(e); }
int WSAAPI WSAStartup(WORD ver, LPWSADATA d) { CALL(int, WSAStartup, (WORD, LPWSADATA), ver, d); }
int WSAAPI WSACleanup(void) { CALL(int, WSACleanup, (void)); }
int WSAAPI __WSAFDIsSet(SOCKET s, fd_set *set) { CALL(int, __WSAFDIsSet, (SOCKET, fd_set *), s, set); }
int WSAAPI WSARecvEx(SOCKET s, char *b, int l, int *f) { CALL(int, WSARecvEx, (SOCKET, char *, int, int *), s, b, l, f); }

/* Winsock 1.1's blocking hooks: no call here ever blocks on one */
__declspec(dllexport) FARPROC WSAAPI WSASetBlockingHook(FARPROC hook) { (void)hook; return 0; }
__declspec(dllexport) int WSAAPI WSAUnhookBlockingHook(void) { return 0; }
__declspec(dllexport) int WSAAPI WSACancelBlockingCall(void) { WSASetLastError(10037 /* WSAEINVAL */); return SOCKET_ERROR; }
__declspec(dllexport) BOOL WSAAPI WSAIsBlocking(void) { return FALSE; }
