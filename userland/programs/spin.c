/* spin.exe — runs until stopped (Ctrl+C), printing once a second */
#include <stdio.h>
#include <windows.h>

int main(void)
{
    for (int i = 1;; i++) {
        volatile unsigned long long x = 0;
        DWORD t = GetTickCount();
        while (GetTickCount() - t < 1000) x++;       /* busy: exercises preemption */
        printf("tick %d (%llu loops)\n", i, (unsigned long long)x);
    }
}
