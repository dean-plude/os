/* crash.exe — writes through a NULL pointer; only this program should die.
 * "crash kernel" instead asks the kernel to fault on purpose
 * (NtNovaBugCheck): the machine halts, and the serial log shows the
 * kernel's backtrace with function names. */
#include <stdio.h>
#include <string.h>
#include <windows.h>

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "kernel")) {
        LONG (WINAPI *bugcheck)(ULONG) = (void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtNovaBugCheck");
        if (!bugcheck) { printf("ntdll has no NtNovaBugCheck\n"); return 1; }
        printf("Asking the kernel to crash...\n");
        fflush(stdout);
        LONG st = bugcheck(0x4E4F5641);                     /* 'NOVA' */
        printf("The kernel did not crash (status 0x%08lx).\n", st);
        return 1;
    }
    printf("About to write to address 0...\n");
    fflush(stdout);
    volatile int *p = (volatile int *)0;
    *p = 42;
    printf("This line is never printed.\n");
    return 0;
}
