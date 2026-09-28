/* crttest.exe — C runtime self-test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <setjmp.h>

static int pass, fail;
#define CHECK(cond) do { if (cond) pass++; else { fail++; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static jmp_buf jb;
static void jump(int v) { longjmp(jb, v); }

int main(void)
{
    char buf[128];
    /* printf */
    snprintf(buf, sizeof(buf), "%d|%5d|%-5d|%05d|%x|%X|%o|%u", -42, 7, 7, 42, 255, 255, 8, 3000000000u);
    CHECK(!strcmp(buf, "-42|    7|7    |00042|ff|FF|10|3000000000"));
    snprintf(buf, sizeof(buf), "%s|%10s|%-4s|%.2s|%c|%%", "abc", "right", "l", "xyz", 'Q');
    CHECK(!strcmp(buf, "abc|     right|l   |xy|Q|%"));
    snprintf(buf, sizeof(buf), "%lld|%llu|%I64d|%zu", -9000000000LL, 18000000000ULL, 123456789012LL, (size_t)77);
    CHECK(!strcmp(buf, "-9000000000|18000000000|123456789012|77"));
    snprintf(buf, sizeof(buf), "%.2f|%.0f|%8.3f|%e|%g|%g|%g", 3.14159, 2.5, -1.5, 12345.678, 0.0001, 1e20, 100.0);
    CHECK(!strcmp(buf, "3.14|3|  -1.500|1.234568e+04|0.0001|1e+20|100"));
    CHECK(snprintf(buf, 4, "hello") == 5 && !strcmp(buf, "hel"));
    /* scanf / conversions */
    int a, b; char w[16]; double d;
    CHECK(sscanf("12 abc 3.5 0x1f", "%d %15s %lf %i", &a, w, &d, &b) == 4 && a == 12 && !strcmp(w, "abc") && d == 3.5 && b == 31);
    CHECK(strtol("-0x10", 0, 0) == -16 && strtoul("777", 0, 8) == 511 && atoi("  42z") == 42);
    CHECK(fabs(strtod("1.5e3", 0) - 1500.0) < 1e-9 && fabs(atof("-0.25") + 0.25) < 1e-12);
    /* strings */
    strcpy(buf, "Hello"); strcat(buf, ", World");
    CHECK(!strcmp(buf, "Hello, World") && strlen(buf) == 12 && strchr(buf, 'W') == buf + 7);
    CHECK(strstr(buf, "World") == buf + 7 && !_stricmp("ABC", "abc") && strncmp("abcd", "abce", 3) == 0);
    char t[] = "a,b,,c"; int n = 0;
    for (char *tok = strtok(t, ","); tok; tok = strtok(0, ",")) n++;
    CHECK(n == 3);
    CHECK(toupper('a') == 'A' && isdigit('7') && !isalpha('1') && isspace('\t'));
    /* memory */
    int *arr = malloc(1000 * sizeof(int));
    for (int i = 0; i < 1000; i++) arr[i] = (i * 7919) % 1000;
    qsort(arr, 1000, sizeof(int), cmp_int);
    int sorted = 1;
    for (int i = 1; i < 1000; i++) if (arr[i - 1] > arr[i]) sorted = 0;
    CHECK(sorted);
    int key = 500;
    CHECK(bsearch(&key, arr, 1000, sizeof(int), cmp_int) != 0);
    arr = realloc(arr, 100000 * sizeof(int));
    CHECK(arr && arr[999] == 999);
    free(arr);
    char *big = calloc(1, 2 * 1024 * 1024);
    CHECK(big && big[123456] == 0);
    free(big);
    void *ptrs[500];
    for (int i = 0; i < 500; i++) ptrs[i] = malloc((size_t)(i * 37 % 3000) + 1);
    for (int i = 0; i < 500; i += 2) free(ptrs[i]);
    for (int i = 0; i < 500; i += 2) ptrs[i] = malloc(64);
    int ok = 1;
    for (int i = 0; i < 500; i++) if (!ptrs[i]) ok = 0;
    for (int i = 0; i < 500; i++) free(ptrs[i]);
    CHECK(ok);
    /* math */
    CHECK(fabs(sqrt(2.0) - 1.4142135623730951) < 1e-15);
    CHECK(fabs(sin(M_PI / 6) - 0.5) < 1e-12 && fabs(cos(M_PI) + 1) < 1e-12);
    CHECK(fabs(exp(1.0) - M_E) < 1e-12 && fabs(log(M_E) - 1) < 1e-12);
    CHECK(fabs(pow(2.0, 10) - 1024) < 1e-12 && fabs(pow(2.0, 0.5) - sqrt(2.0)) < 1e-12);
    CHECK(fabs(atan2(1, 1) - M_PI / 4) < 1e-12 && floor(-1.5) == -2 && ceil(1.2) == 2);
    /* time */
    time_t now = time(0);
    struct tm *tm = gmtime(&now);
    CHECK(now > 1600000000 && tm->tm_year >= 120);
    strftime(buf, sizeof(buf), "%Y-%m-%d", tm);
    CHECK(strlen(buf) == 10);
    /* setjmp/longjmp */
    volatile int jumped = 0;
    int r = setjmp(jb);
    if (!r) jump(7); else jumped = r;
    CHECK(jumped == 7);
    /* errno */
    errno = 0;
    FILE *f = fopen("C:\\no\\such\\file.txt", "r");
    CHECK(!f && errno == ENOENT);

    printf("crttest: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
