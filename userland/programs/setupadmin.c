/* setupadmin.exe — setuptest's helper: its manifest asks to run as
 * administrator, so NovaOS starts it elevated.  Exits 0 when its token is
 * elevated (TokenElevation, Administrators enabled, high integrity), 1 if
 * not. */
#include <stdio.h>
#include <windows.h>

int main(void)
{
    HANDLE t;
    DWORD elev = 0, type = 0, n;
    BOOL admin = FALSE;
    SID_IDENTIFIER_AUTHORITY nt = { SECURITY_NT_AUTHORITY };
    PSID sid;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return 2;
    GetTokenInformation(t, TokenElevation, &elev, sizeof(elev), &n);
    GetTokenInformation(t, TokenElevationType, &type, sizeof(type), &n);
    CloseHandle(t);
    if (AllocateAndInitializeSid(&nt, 2, 32, 544, 0, 0, 0, 0, 0, 0, &sid)) {   /* Administrators, S-1-5-32-544 */
        CheckTokenMembership(NULL, sid, &admin);
        FreeSid(sid);
    }
    printf("setupadmin: elevation %lu, type %lu, administrator %d\n", elev, type, admin);
    return elev && type == 2 && admin ? 0 : 1;
}
