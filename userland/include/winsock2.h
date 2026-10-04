/* winsock2.h — the Winsock 2 subset NovaOS's ws2_32.dll implements */
#pragma once
#include <windows.h>
#include <errno.h>
_NOVA_BEGIN

#ifdef WS2_EXPORT
#define WSAAPI_DECL __declspec(dllexport) __stdcall
#else
#define WSAAPI_DECL __declspec(dllimport) __stdcall
#endif
#define WSAAPI __stdcall

typedef UINT_PTR SOCKET;
#define INVALID_SOCKET  (~(SOCKET)0)
#define SOCKET_ERROR    (-1)

#define AF_UNSPEC   0
#define AF_INET     2
#define AF_INET6    23
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define SOMAXCONN   0x7fffffff

#define SD_RECEIVE 0
#define SD_SEND    1
#define SD_BOTH    2

#define SOL_SOCKET   0xffff
#define SO_REUSEADDR 0x0004
#define SO_KEEPALIVE 0x0008
#define SO_BROADCAST 0x0020
#define SO_LINGER    0x0080
#define SO_SNDBUF    0x1001
#define SO_RCVBUF    0x1002
#define SO_ERROR     0x1007
#define SO_TYPE      0x1008
#define SO_ACCEPTCONN 0x0002
#define SO_DONTLINGER ((int)(~SO_LINGER))
#define SO_SNDTIMEO  0x1005
#define SO_RCVTIMEO  0x1006
#define TCP_NODELAY  0x0001
#define IPPROTO_IP   0
#define IP_TTL       4
struct linger { USHORT l_onoff; USHORT l_linger; };
typedef struct linger LINGER, *PLINGER, *LPLINGER;
#define FIONBIO      0x8004667e
#define FIONREAD     0x4004667f

#define INADDR_ANY        0x00000000
#define INADDR_LOOPBACK   0x7f000001
#define INADDR_NONE       0xffffffff
#define INADDR_BROADCAST  0xffffffff

#define WSADESCRIPTION_LEN 256
#define WSASYS_STATUS_LEN  128

#define WSAEWOULDBLOCK     10035
#define WSAEINPROGRESS     10036
#define WSAEALREADY        10037
#define WSAENOTSOCK        10038
#define WSAEADDRINUSE      10048
#define WSAEADDRNOTAVAIL   10049
#define WSAENETDOWN        10050
#define WSAENETUNREACH     10051
#define WSAECONNABORTED    10053
#define WSAECONNRESET      10054
#define WSAENOBUFS         10055
#define WSAEISCONN         10056
#define WSAENOTCONN        10057
#define WSAETIMEDOUT       10060
#define WSAECONNREFUSED    10061
#define WSAEHOSTUNREACH    10065
#define WSAHOST_NOT_FOUND  11001
#define WSATRY_AGAIN       11002
#define WSANO_DATA         11004
#define WSANOTINITIALISED  10093
#define WSAEFAULT          10014
#define WSAEINVAL          10022
#define WSAEMFILE          10024
#define WSAEAFNOSUPPORT    10047
#define WSAENOPROTOOPT     10042

typedef struct in_addr { union { struct { UCHAR s_b1,s_b2,s_b3,s_b4; } S_un_b; ULONG S_addr; } S_un;
#define s_addr S_un.S_addr
} IN_ADDR, *PIN_ADDR;
struct sockaddr { USHORT sa_family; CHAR sa_data[14]; };
struct sockaddr_in { SHORT sin_family; USHORT sin_port; struct in_addr sin_addr; CHAR sin_zero[8]; };
typedef struct in6_addr { union { UCHAR Byte[16]; USHORT Word[8]; } u; } IN6_ADDR, *PIN6_ADDR;
#define s6_addr   u.Byte
#define s6_words  u.Word
struct sockaddr_in6 {
    SHORT sin6_family; USHORT sin6_port; ULONG sin6_flowinfo;
    struct in6_addr sin6_addr; ULONG sin6_scope_id;
};
typedef struct sockaddr_in6 SOCKADDR_IN6, *PSOCKADDR_IN6;
struct sockaddr_storage { SHORT ss_family; CHAR __ss_pad1[6]; __int64 __ss_align; CHAR __ss_pad2[112]; };
typedef struct sockaddr_storage SOCKADDR_STORAGE, *PSOCKADDR_STORAGE;
#define IN6ADDR_ANY_INIT        { { { 0 } } }
#define IN6ADDR_LOOPBACK_INIT   { { { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1 } } }
static const struct in6_addr in6addr_any = IN6ADDR_ANY_INIT;
static const struct in6_addr in6addr_loopback = IN6ADDR_LOOPBACK_INIT;
#define IN6_IS_ADDR_V4MAPPED(a) (!(a)->s6_words[0] && !(a)->s6_words[1] && !(a)->s6_words[2] && \
                                 !(a)->s6_words[3] && !(a)->s6_words[4] && (a)->s6_words[5] == 0xFFFF)
#define IN6_IS_ADDR_LINKLOCAL(a) ((a)->s6_addr[0] == 0xFE && ((a)->s6_addr[1] & 0xC0) == 0x80)
#define IPPROTO_IPV6     41
#define IPV6_V6ONLY      27
#define IPV6_UNICAST_HOPS 4
#define INET6_ADDRSTRLEN 65
typedef struct sockaddr SOCKADDR, *PSOCKADDR, *LPSOCKADDR;
typedef struct sockaddr_in SOCKADDR_IN, *PSOCKADDR_IN;
typedef int socklen_t;

typedef struct WSAData {
    WORD wVersion, wHighVersion;
    CHAR szDescription[WSADESCRIPTION_LEN + 1];
    CHAR szSystemStatus[WSASYS_STATUS_LEN + 1];
    unsigned short iMaxSockets, iMaxUdpDg;
    char *lpVendorInfo;
} WSADATA, *LPWSADATA;
#define MAKEWORD(a, b) ((WORD)(((BYTE)(a)) | ((WORD)((BYTE)(b))) << 8))

/* struct servent: s_proto and s_port swap places on 64-bit Windows */
typedef struct servent {
    char *s_name;
    char **s_aliases;
#if defined(__x86_64__) || defined(_M_X64)
    char *s_proto;
    short s_port;
#else
    short s_port;
    char *s_proto;
#endif
} SERVENT, *PSERVENT;

typedef struct hostent {
    char *h_name; char **h_aliases;
    short h_addrtype, h_length;
    char **h_addr_list;
} HOSTENT, *LPHOSTENT;
#define h_addr h_addr_list[0]

struct addrinfo {
    int ai_flags, ai_family, ai_socktype, ai_protocol;
    size_t ai_addrlen;
    char *ai_canonname;
    struct sockaddr *ai_addr;
    struct addrinfo *ai_next;
};
typedef struct addrinfo ADDRINFOA, *PADDRINFOA;
#define AI_PASSIVE      0x01
#define AI_CANONNAME    0x02
#define AI_NUMERICHOST  0x04
#define AI_NUMERICSERV  0x08
#define AI_ALL          0x0100
#define AI_ADDRCONFIG   0x0400
#define AI_V4MAPPED     0x0800
#define EAI_NONAME      WSAHOST_NOT_FOUND
#define EAI_FAMILY      WSAEAFNOSUPPORT

#ifndef FD_SETSIZE
#define FD_SETSIZE 64                 /* (a program may define it larger first, as on Windows) */
#endif
typedef struct fd_set { UINT fd_count; SOCKET fd_array[FD_SETSIZE]; } fd_set;
#ifndef _NOVA_TIMEVAL
#define _NOVA_TIMEVAL
struct timeval { long tv_sec; long tv_usec; };
#endif
typedef unsigned int u_int;
typedef unsigned long u_long;
typedef unsigned short u_short;

int WSAAPI FD_ISSET(SOCKET, fd_set *);
#define FD_ZERO(s)     ((s)->fd_count = 0)
#define FD_SET(fd, s)  do { if ((s)->fd_count < FD_SETSIZE) (s)->fd_array[(s)->fd_count++] = (fd); } while (0)

WSAAPI_DECL int WSAStartup(WORD ver, LPWSADATA data);
WSAAPI_DECL int WSACleanup(void);
WSAAPI_DECL int WSAGetLastError(void);
WSAAPI_DECL void WSASetLastError(int err);
WSAAPI_DECL SOCKET socket(int af, int type, int protocol);
WSAAPI_DECL int closesocket(SOCKET s);
WSAAPI_DECL int connect(SOCKET s, const struct sockaddr *name, int namelen);
WSAAPI_DECL int bind(SOCKET s, const struct sockaddr *name, int namelen);
WSAAPI_DECL int listen(SOCKET s, int backlog);
WSAAPI_DECL SOCKET accept(SOCKET s, struct sockaddr *addr, int *addrlen);
WSAAPI_DECL int send(SOCKET s, const char *buf, int len, int flags);
WSAAPI_DECL int recv(SOCKET s, char *buf, int len, int flags);
WSAAPI_DECL int sendto(SOCKET s, const char *buf, int len, int flags, const struct sockaddr *to, int tolen);
WSAAPI_DECL int recvfrom(SOCKET s, char *buf, int len, int flags, struct sockaddr *from, int *fromlen);
WSAAPI_DECL int shutdown(SOCKET s, int how);
WSAAPI_DECL int setsockopt(SOCKET s, int level, int optname, const char *optval, int optlen);
WSAAPI_DECL int getsockopt(SOCKET s, int level, int optname, char *optval, int *optlen);
WSAAPI_DECL int ioctlsocket(SOCKET s, long cmd, u_long *argp);
WSAAPI_DECL int getpeername(SOCKET s, struct sockaddr *name, int *namelen);
WSAAPI_DECL int getsockname(SOCKET s, struct sockaddr *name, int *namelen);
WSAAPI_DECL int select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, const struct timeval *timeout);
WSAAPI_DECL u_short htons(u_short v);
WSAAPI_DECL u_short ntohs(u_short v);
WSAAPI_DECL u_long htonl(u_long v);
WSAAPI_DECL u_long ntohl(u_long v);
WSAAPI_DECL unsigned long inet_addr(const char *cp);
WSAAPI_DECL char *inet_ntoa(struct in_addr in);
WSAAPI_DECL struct hostent *gethostbyname(const char *name);
WSAAPI_DECL int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
WSAAPI_DECL void freeaddrinfo(struct addrinfo *ai);
WSAAPI_DECL int __WSAFDIsSet(SOCKET fd, fd_set *set);

/* ---- Winsock 2 extensions (overlapped operations complete at once) ---- */
typedef struct _WSABUF { ULONG len; CHAR *buf; } WSABUF, *LPWSABUF;
typedef struct _WSAMSG {
    struct sockaddr *name; INT namelen; LPWSABUF lpBuffers; ULONG dwBufferCount; WSABUF Control; ULONG dwFlags;
} WSAMSG, *PWSAMSG, *LPWSAMSG;
typedef OVERLAPPED WSAOVERLAPPED, *LPWSAOVERLAPPED;
typedef void (WINAPI *LPWSAOVERLAPPED_COMPLETION_ROUTINE)(DWORD err, DWORD bytes, LPWSAOVERLAPPED ov, DWORD flags);
typedef HANDLE WSAEVENT;
typedef unsigned int GROUP;
typedef struct _WSAPROTOCOL_INFOW WSAPROTOCOL_INFOW, *LPWSAPROTOCOL_INFOW;
typedef struct _WSAPROTOCOL_INFOA WSAPROTOCOL_INFOA, *LPWSAPROTOCOL_INFOA;
typedef struct addrinfoW {
    int ai_flags, ai_family, ai_socktype, ai_protocol;
    size_t ai_addrlen;
    PWSTR ai_canonname;
    struct sockaddr *ai_addr;
    struct addrinfoW *ai_next;
} ADDRINFOW, *PADDRINFOW;
#define WSA_INVALID_EVENT      ((WSAEVENT)0)
#define WSA_IO_PENDING         997
#define WSA_IO_INCOMPLETE      996
#define WSA_WAIT_EVENT_0       0
#define WSA_WAIT_TIMEOUT       258
#define WSA_WAIT_FAILED        0xFFFFFFFF
#define WSA_INFINITE           0xFFFFFFFF
#define WSA_FLAG_OVERLAPPED    0x01
#define WSA_FLAG_NO_HANDLE_INHERIT 0x80
#define WSAEOPNOTSUPP          10045
#define WSANO_DATA             11004
#define WSAHOST_NOT_FOUND      11001
#define SIO_GET_EXTENSION_FUNCTION_POINTER 0xC8000006
#define SIO_KEEPALIVE_VALS     0x98000004
#define INET_ADDRSTRLEN        16

WSAAPI_DECL SOCKET WSASocketW(int af, int type, int protocol, LPWSAPROTOCOL_INFOW info, GROUP g, DWORD flags);
WSAAPI_DECL SOCKET WSASocketA(int af, int type, int protocol, LPWSAPROTOCOL_INFOA info, GROUP g, DWORD flags);
WSAAPI_DECL int WSASend(SOCKET s, LPWSABUF bufs, DWORD n, LPDWORD sent, DWORD flags, LPWSAOVERLAPPED ov,
                        LPWSAOVERLAPPED_COMPLETION_ROUTINE cr);
WSAAPI_DECL int WSARecv(SOCKET s, LPWSABUF bufs, DWORD n, LPDWORD got, LPDWORD flags, LPWSAOVERLAPPED ov,
                        LPWSAOVERLAPPED_COMPLETION_ROUTINE cr);
WSAAPI_DECL int WSASendMsg(SOCKET s, LPWSAMSG msg, DWORD flags, LPDWORD sent, LPWSAOVERLAPPED ov,
                           LPWSAOVERLAPPED_COMPLETION_ROUTINE cr);
WSAAPI_DECL int WSASendTo(SOCKET s, LPWSABUF bufs, DWORD n, LPDWORD sent, DWORD flags, const struct sockaddr *to, int tolen,
                          LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr);
WSAAPI_DECL int WSARecvFrom(SOCKET s, LPWSABUF bufs, DWORD n, LPDWORD got, LPDWORD flags, struct sockaddr *from, int *fromlen,
                            LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr);
WSAAPI_DECL BOOL WSAGetOverlappedResult(SOCKET s, LPWSAOVERLAPPED ov, LPDWORD bytes, BOOL wait, LPDWORD flags);
WSAAPI_DECL int WSAIoctl(SOCKET s, DWORD code, LPVOID in, DWORD inlen, LPVOID out, DWORD outlen, LPDWORD ret,
                         LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr);
WSAAPI_DECL WSAEVENT WSACreateEvent(void);
#ifndef FD_READ
#define FD_READ     0x01
#define FD_WRITE    0x02
#define FD_OOB      0x04
#define FD_ACCEPT   0x08
#define FD_CONNECT  0x10
#define FD_CLOSE    0x20
#define FD_MAX_EVENTS 10
#endif
#ifndef MSG_PEEK
#define MSG_PEEK    0x2
#endif
typedef struct { long lNetworkEvents; int iErrorCode[FD_MAX_EVENTS]; } WSANETWORKEVENTS, *LPWSANETWORKEVENTS;
WSAAPI_DECL int WSAEventSelect(SOCKET s, WSAEVENT ev, long events);
WSAAPI_DECL int WSAAsyncSelect(SOCKET s, HWND hwnd, unsigned int msg, long events);
#define WSAGETSELECTEVENT(l) LOWORD(l)
#define WSAGETSELECTERROR(l) HIWORD(l)
WSAAPI_DECL int WSAEnumNetworkEvents(SOCKET s, WSAEVENT ev, LPWSANETWORKEVENTS out);
WSAAPI_DECL int WSAAddressToStringA(struct sockaddr *sa, DWORD len, void *info, char *out, DWORD *outlen);
WSAAPI_DECL int WSAAddressToStringW(struct sockaddr *sa, DWORD len, void *info, WCHAR *out, DWORD *outlen);
#define NI_NOFQDN      0x01
#define NI_NUMERICHOST 0x02
#define NI_NAMEREQD    0x04
#define NI_NUMERICSERV 0x08
#define NI_DGRAM       0x10
#define NI_MAXHOST     1025
#define NI_MAXSERV     32
WSAAPI_DECL int getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host, DWORD hostlen, char *serv, DWORD servlen, int flags);
WSAAPI_DECL int GetNameInfoW(const struct sockaddr *sa, socklen_t salen, WCHAR *host, DWORD hostlen, WCHAR *serv, DWORD servlen, int flags);
WSAAPI_DECL int WSADuplicateSocketW(SOCKET s, DWORD pid, void *info);
WSAAPI_DECL int WSADuplicateSocketA(SOCKET s, DWORD pid, void *info);
WSAAPI_DECL int WSAConnect(SOCKET s, const struct sockaddr *to, int len, void *caller, void *callee, void *sqos, void *gqos);
WSAAPI_DECL int WSAStringToAddressA(char *str, int family, void *info, struct sockaddr *sa, int *len);
WSAAPI_DECL int WSAStringToAddressW(WCHAR *str, int family, void *info, struct sockaddr *sa, int *len);
WSAAPI_DECL BOOL WSACloseEvent(WSAEVENT e);
WSAAPI_DECL BOOL WSASetEvent(WSAEVENT e);
WSAAPI_DECL BOOL WSAResetEvent(WSAEVENT e);
WSAAPI_DECL DWORD WSAWaitForMultipleEvents(DWORD n, const WSAEVENT *events, BOOL all, DWORD ms, BOOL alertable);
WSAAPI_DECL int GetAddrInfoW(PCWSTR node, PCWSTR service, const ADDRINFOW *hints, PADDRINFOW *res);
WSAAPI_DECL void FreeAddrInfoW(PADDRINFOW ai);
WSAAPI_DECL int inet_pton(int af, const char *src, void *dst);
WSAAPI_DECL const char *inet_ntop(int af, const void *src, char *dst, size_t size);
WSAAPI_DECL int InetPtonW(int af, PCWSTR src, void *dst);
WSAAPI_DECL PCWSTR InetNtopW(int af, const void *src, PWSTR dst, size_t size);
#ifndef _NOVA_GETHOSTNAME                   /* unistd.h declares the POSIX one (msvcrt) */
#define _NOVA_GETHOSTNAME
WSAAPI_DECL int gethostname(char *name, int len);
#endif
WSAAPI_DECL int GetHostNameW(PWSTR name, int len);
WSAAPI_DECL struct servent *getservbyname(const char *name, const char *proto);
WSAAPI_DECL int WSAEnumProtocolsW(int *protocols, LPWSAPROTOCOL_INFOW buf, LPDWORD len);
WSAAPI_DECL int WSAEnumProtocolsA(int *protocols, LPWSAPROTOCOL_INFOA buf, LPDWORD len);

_NOVA_END
