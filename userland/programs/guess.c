/* guess.exe — interactive: reads the keyboard through stdin */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(void)
{
    srand((unsigned)time(0));
    int secret = rand() % 100 + 1, tries = 0, g;
    char line[64];
    printf("I'm thinking of a number between 1 and 100.\n");
    for (;;) {
        printf("Your guess: ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) { printf("\nBye!\n"); return 1; }
        if (sscanf(line, "%d", &g) != 1) { printf("That's not a number.\n"); continue; }
        tries++;
        if (g < secret) printf("Higher!\n");
        else if (g > secret) printf("Lower!\n");
        else { printf("Correct! You got it in %d tries.\n", tries); return 0; }
    }
}
