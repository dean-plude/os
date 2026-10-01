/* posixtest.exe — self-test of msvcrt's POSIX layer (what the NetSurf port
 * relies on): descriptors, dup/fdopen, stat, directories, getopt, iconv,
 * gettimeofday, asprintf, static constructors and kernel random bytes */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <getopt.h>
#include <iconv.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <windows.h>
#include <winternl.h>

static int pass, fail;
#define CHECK(cond) do { if (cond) pass++; else { fail++; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static int g_ctor_ran;
__attribute__((constructor)) static void ctor(void) { g_ctor_ran = 42; }

int main(void)
{
    /* static constructors run before main (crt0) */
    CHECK(g_ctor_ran == 42);

    /* descriptors */
    mkdir("\\Temp\\ptest", 0755);
    int fd = open("\\Temp\\ptest\\a.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 3);
    CHECK(write(fd, "hello world", 11) == 11);
    CHECK(pwrite(fd, "W", 1, 6) == 1 && lseek(fd, 0, SEEK_CUR) == 11);
    CHECK(close(fd) == 0 && close(fd) == -1 && errno == EBADF);
    fd = open("/Temp/ptest/a.txt", O_RDONLY);          /* '/' separators work too */
    char buf[64] = { 0 };
    CHECK(fd >= 0 && read(fd, buf, sizeof(buf)) == 11 && !strcmp(buf, "hello World"));
    CHECK(pread(fd, buf, 5, 6) == 5 && !memcmp(buf, "World", 5));
    CHECK(lseek(fd, -5, SEEK_END) == 6);
    int fd2 = dup(fd);
    CHECK(fd2 >= 0 && fd2 != fd && lseek(fd2, 0, SEEK_CUR) == 6);   /* shared offset */
    close(fd);
    FILE *f = fdopen(fd2, "r");
    CHECK(f && fgets(buf, sizeof(buf), f) && !strcmp(buf, "World"));
    CHECK(fclose(f) == 0);
    CHECK(access("\\Temp\\ptest\\a.txt", F_OK) == 0 && access("\\Temp\\ptest\\none", F_OK) == -1 && errno == ENOENT);
    f = fopen("\\Temp\\ptest\\b.txt", "w");
    CHECK(f && fileno(f) >= 3);
    fputs("xyz", f);
    fclose(f);

    /* stat */
    struct stat st;
    CHECK(stat("\\Temp\\ptest\\a.txt", &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 11);
    CHECK(stat("\\Temp\\ptest", &st) == 0 && S_ISDIR(st.st_mode));
    CHECK(stat("\\Temp\\ptest\\none", &st) == -1 && errno == ENOENT);

    /* directories */
    DIR *d = opendir("\\Temp\\ptest");
    int seen = 0;
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (!strcmp(e->d_name, "a.txt") || !strcmp(e->d_name, "b.txt")) seen++;
    CHECK(d && seen == 2 && closedir(d) == 0);
    struct dirent **list = NULL;
    int n = scandir("\\Temp\\ptest", &list, NULL, alphasort);
    CHECK(n == 4 && !strcmp(list[0]->d_name, ".") && !strcmp(list[1]->d_name, "..") &&
          !strcmp(list[2]->d_name, "a.txt") && !strcmp(list[3]->d_name, "b.txt"));
    while (n > 0) free(list[--n]);
    free(list);
    CHECK(unlink("\\Temp\\ptest\\a.txt") == 0 && unlink("\\Temp\\ptest\\b.txt") == 0);
    CHECK(rmdir("\\Temp\\ptest") == 0 && opendir("\\Temp\\ptest") == NULL);

    /* getopt_long */
    static struct option lo[] = { { "width", required_argument, 0, 'w' }, { "quiet", no_argument, 0, 'q' }, { 0 } };
    char *argv[] = { "prog", "-b32", "--width=640", "-h", "480", "--quiet", "url", NULL };
    int argc = 7, c, bpp = 0, wdt = 0, hgt = 0, quiet = 0, idx;
    optind = 1;
    while ((c = getopt_long(argc, argv, "b:h:w:", lo, &idx)) != -1) {
        if (c == 'b') bpp = atoi(optarg);
        else if (c == 'w') wdt = atoi(optarg);
        else if (c == 'h') hgt = atoi(optarg);
        else if (c == 'q') quiet = 1;
    }
    CHECK(bpp == 32 && wdt == 640 && hgt == 480 && quiet == 1 && optind == 6 && !strcmp(argv[optind], "url"));

    /* iconv */
    iconv_t cd = iconv_open("UTF-8", "ISO-8859-1");
    char in[] = "caf\xe9", out[16] = { 0 };
    char *ip = in, *op = out;
    size_t il = 4, ol = sizeof(out);
    CHECK(cd != (iconv_t)-1 && iconv(cd, &ip, &il, &op, &ol) == 0 && !strcmp(out, "caf\xc3\xa9") && il == 0);
    iconv_close(cd);
    cd = iconv_open("UTF-16LE", "UTF-8");
    char u8[] = "A\xe2\x82\xac", u16[8];
    ip = u8; il = 4; op = u16; ol = sizeof(u16);
    CHECK(iconv(cd, &ip, &il, &op, &ol) == 0 && ol == 4 && !memcmp(u16, "A\0\xac\x20", 4));
    iconv_close(cd);
    cd = iconv_open("UTF-8", "UTF-8");
    char bad[] = "a\xff";
    ip = bad; il = 2; op = out; ol = sizeof(out);
    CHECK(iconv(cd, &ip, &il, &op, &ol) == (size_t)-1 && errno == EILSEQ && il == 1);
    iconv_close(cd);
    CHECK(iconv_open("KLINGON", "UTF-8") == (iconv_t)-1 && errno == EINVAL);

    /* time, formatting, randomness */
    struct timeval tv;
    CHECK(gettimeofday(&tv, NULL) == 0 && tv.tv_sec > 1700000000 && tv.tv_usec >= 0 && tv.tv_usec < 1000000);
    char *s = NULL;
    CHECK(asprintf(&s, "%s-%d", "nova", 7) == 6 && s && !strcmp(s, "nova-7"));
    free(s);
    unsigned char r1[32] = { 0 }, r2[32] = { 0 };
    CHECK(NT_SUCCESS(NtNovaGetRandom(r1, sizeof(r1))) && NT_SUCCESS(NtNovaGetRandom(r2, sizeof(r2))) &&
          memcmp(r1, r2, sizeof(r1)) != 0);

    printf("posixtest: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
