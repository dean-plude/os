/* bcryptprimitives.dll — ProcessPrng, the random source Rust's standard
 * library and others use: bytes from the kernel's entropy pool */
#include <winternl.h>

__declspec(dllexport) BOOL WINAPI ProcessPrng(PUCHAR buf, SIZE_T n)
{
    while (n) {
        ULONG k = n > 0x10000 ? 0x10000 : (ULONG)n;
        NtNovaGetRandom(buf, k);
        buf += k;
        n -= k;
    }
    return TRUE;                            /* documented never to fail */
}
