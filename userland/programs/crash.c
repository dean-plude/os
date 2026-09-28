/* crash.exe — writes through a NULL pointer; only this program should die */
#include <stdio.h>

int main(void)
{
    printf("About to write to address 0...\n");
    fflush(stdout);
    volatile int *p = (volatile int *)0;
    *p = 42;
    printf("This line is never printed.\n");
    return 0;
}
