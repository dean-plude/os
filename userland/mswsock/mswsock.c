/*
 * mswsock.dll — Microsoft's Winsock extensions.  TransmitFile sends a
 * file (or part of it) over a connected socket by reading and sending.
 */
#include <windows.h>
#include <winsock2.h>

#define MSWSOCKAPI __declspec(dllexport)

typedef struct { PVOID Head; DWORD HeadLength; PVOID Tail; DWORD TailLength; } TF_BUFFERS_;

static BOOL send_all(SOCKET s, const char *p, DWORD n)
{
    while (n) {
        int k = send(s, p, n > 65536 ? 65536 : (int)n, 0);
        if (k <= 0) return FALSE;
        p += k;
        n -= (DWORD)k;
    }
    return TRUE;
}

MSWSOCKAPI BOOL WINAPI TransmitFile(SOCKET s, HANDLE f, DWORD total, DWORD per, LPOVERLAPPED ov, TF_BUFFERS_ *bufs, DWORD flags)
{
    (void)per; (void)flags;
    if (ov) {                                        /* from the offset the OVERLAPPED gives */
        LARGE_INTEGER at;
        at.LowPart = ov->Offset;
        at.HighPart = (LONG)ov->OffsetHigh;
        if (!SetFilePointerEx(f, at, 0, FILE_BEGIN)) return FALSE;
    }
    if (bufs && bufs->Head && bufs->HeadLength && !send_all(s, bufs->Head, bufs->HeadLength)) return FALSE;
    char *buf = HeapAlloc(GetProcessHeap(), 0, 65536);
    if (!buf) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    BOOL ok = TRUE;
    DWORD left = total ? total : 0xFFFFFFFF;
    while (left) {
        DWORD got = 0;
        if (!ReadFile(f, buf, left < 65536 ? left : 65536, &got, 0)) { ok = FALSE; break; }
        if (!got) break;
        if (!send_all(s, buf, got)) { ok = FALSE; break; }
        left -= got;
    }
    HeapFree(GetProcessHeap(), 0, buf);
    if (ok && bufs && bufs->Tail && bufs->TailLength) ok = send_all(s, bufs->Tail, bufs->TailLength);
    if (ok && ov && ov->hEvent) SetEvent(ov->hEvent);
    return ok;
}

/* AcceptEx and GetAcceptExSockaddrs: ws2_32's (the ones WSAIoctl's
 * SIO_GET_EXTENSION_FUNCTION_POINTER hands out) */
typedef BOOL (WINAPI *AcceptEx_t)(SOCKET, SOCKET, PVOID, DWORD, DWORD, DWORD, LPDWORD, LPOVERLAPPED);
MSWSOCKAPI BOOL WINAPI AcceptEx(SOCKET l, SOCKET a, PVOID buf, DWORD n, DWORD ll, DWORD rl, LPDWORD got, LPOVERLAPPED ov)
{
    static const GUID id = { 0xb5367df1, 0xcbac, 0x11cf, { 0x95, 0xca, 0x00, 0x80, 0x5f, 0x48, 0xa1, 0x92 } };
    AcceptEx_t f = 0;
    DWORD r;
    if (WSAIoctl(l, SIO_GET_EXTENSION_FUNCTION_POINTER, (void *)&id, sizeof(id), &f, sizeof(f), &r, 0, 0)) return FALSE;
    return f(l, a, buf, n, ll, rl, got, ov);
}
MSWSOCKAPI VOID WINAPI GetAcceptExSockaddrs(PVOID buf, DWORD n, DWORD ll, DWORD rl, struct sockaddr **la, LPINT lal,
                                           struct sockaddr **ra, LPINT ral)
{
    *la = (struct sockaddr *)((char *)buf + n);
    *ra = (struct sockaddr *)((char *)buf + n + ll);
    *lal = (*la)->sa_family == AF_INET6 ? (INT)sizeof(struct sockaddr_in6) : (INT)sizeof(struct sockaddr_in);
    *ral = (*ra)->sa_family == AF_INET6 ? (INT)sizeof(struct sockaddr_in6) : (INT)sizeof(struct sockaddr_in);
}
