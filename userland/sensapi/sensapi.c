/*
 * sensapi.dll — network connectivity checks.  The answer comes from the
 * network adapter's state (iphlpapi's view: an adapter with an address).
 */
#include <windows.h>
#include <winsock2.h>

#define SENSAPI __declspec(dllexport)
#define NETWORK_ALIVE_LAN_ 1

/* Whether this machine has an IPv4 address: the local host name resolves
 * to something other than a loopback address */
static BOOL connected(void)
{
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd)) return FALSE;
    char name[256];
    BOOL up = FALSE;
    if (!gethostname(name, sizeof(name))) {
        struct hostent *h = gethostbyname(name);
        for (int i = 0; h && h->h_addr_list[i]; i++)
            if ((unsigned char)h->h_addr_list[i][0] != 127) up = TRUE;
    }
    WSACleanup();
    return up;
}

SENSAPI BOOL WINAPI IsNetworkAlive(LPDWORD flags)
{
    BOOL up = connected();
    if (flags) *flags = up ? NETWORK_ALIVE_LAN_ : 0;
    SetLastError(0);
    return up;
}
SENSAPI BOOL WINAPI IsDestinationReachableW(LPCWSTR dest, LPVOID qoc) { (void)dest; (void)qoc; return IsNetworkAlive(NULL); }
SENSAPI BOOL WINAPI IsDestinationReachableA(LPCSTR dest, LPVOID qoc) { (void)dest; (void)qoc; return IsNetworkAlive(NULL); }
