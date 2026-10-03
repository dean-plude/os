/* rttest.exe — runtime pieces Python extensions and C++ programs lean on:
 * the C99 complex functions in ucrtbase (with MSVC's _Dcomplex and
 * _Fcomplex structures, as NumPy calls them), conio's _cprintf family, and
 * the DLL search directories (AddDllDirectory, RemoveDllDirectory,
 * SetDllDirectory) */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <windows.h>

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

/* <complex.h>'s types as the UCRT declares them */
typedef struct { double _Val[2]; } _Dcomplex;
typedef struct { float _Val[2]; } _Fcomplex;
__declspec(dllimport) _Dcomplex _Cbuild(double, double);
__declspec(dllimport) _Fcomplex _FCbuild(float, float);
__declspec(dllimport) double creal(_Dcomplex);
__declspec(dllimport) double cimag(_Dcomplex);
__declspec(dllimport) double cabs(_Dcomplex);
__declspec(dllimport) double carg(_Dcomplex);
__declspec(dllimport) _Dcomplex cexp(_Dcomplex);
__declspec(dllimport) _Dcomplex clog(_Dcomplex);
__declspec(dllimport) _Dcomplex csqrt(_Dcomplex);
__declspec(dllimport) _Dcomplex csin(_Dcomplex);
__declspec(dllimport) _Dcomplex ctanh(_Dcomplex);
__declspec(dllimport) _Dcomplex cpow(_Dcomplex, _Dcomplex);
__declspec(dllimport) _Dcomplex _Cmulcc(_Dcomplex, _Dcomplex);
__declspec(dllimport) _Fcomplex cexpf(_Fcomplex);
__declspec(dllimport) float cabsf(_Fcomplex);
__declspec(dllimport) int _cprintf(const char *, ...);
__declspec(dllimport) int _cputs(const char *);

static int near(double a, double b) { return fabs(a - b) <= 1e-9 * (1 + fabs(b)); }
static int cnear(_Dcomplex z, double re, double im) { return near(z._Val[0], re) && near(z._Val[1], im); }

static void complex_math(void)
{
    _Dcomplex z = _Cbuild(1, 2);
    CHECK("_Cbuild/creal/cimag", creal(z) == 1 && cimag(z) == 2);
    CHECK("cabs", near(cabs(_Cbuild(3, -4)), 5));
    CHECK("carg", near(carg(_Cbuild(0, 1)), 1.5707963267948966));
    CHECK("cexp", cnear(cexp(z), -1.1312043837568135, 2.4717266720048188));
    CHECK("clog", cnear(clog(z), 0.8047189562170502, 1.1071487177940904));
    CHECK("csqrt", cnear(csqrt(_Cbuild(-4, 0)), 0, 2));
    CHECK("csin", cnear(csin(z), 3.165778513216168, 1.9596010414216063));
    CHECK("ctanh", cnear(ctanh(z), 1.16673625724092, -0.24345820118572534));
    CHECK("cpow", cnear(cpow(_Cbuild(0, 1), _Cbuild(2, 0)), -1, 0));
    CHECK("_Cmulcc", cnear(_Cmulcc(z, _Cbuild(3, -4)), 11, 2));
    _Fcomplex f = cexpf(_FCbuild(0, 3.14159265f));
    CHECK("cexpf", fabs(f._Val[0] + 1) < 1e-5 && fabs(f._Val[1]) < 1e-5);
    CHECK("cabsf", fabsf(cabsf(_FCbuild(3, 4)) - 5) < 1e-6f);
}

static void conio(void)
{
    CHECK("_cprintf", _cprintf("rttest: %s %d\r\n", "cprintf", 42) == 20);
    CHECK("_cputs", _cputs("rttest: cputs\r\n") == 0);
}

/* testdll.dll copied into a directory of its own under several names (a
 * module stays loaded once found, so each check looks for a fresh name) */
static void dll_directories(void)
{
    WCHAR src[MAX_PATH], dir[MAX_PATH], dst[MAX_PATH];
    static const WCHAR *names[] = { L"dirtest1.dll", L"dirtest2.dll", L"dirtest3.dll", L"dirtest4.dll" };
    HMODULE t = LoadLibraryW(L"testdll.dll");
    CHECK("testdll loads", t != NULL);
    GetModuleFileNameW(t, src, MAX_PATH);
    GetTempPathW(MAX_PATH, dir);
    lstrcatW(dir, L"rttest-dlls");
    CreateDirectoryW(dir, NULL);
    for (int i = 0; i < 4; i++) {
        lstrcpyW(dst, dir);
        lstrcatW(dst, L"\\");
        lstrcatW(dst, names[i]);
        CHECK("copy testdll", CopyFileW(src, dst, FALSE));
    }

    CHECK("not found before", LoadLibraryW(L"dirtest1.dll") == NULL);
    CHECK("AddDllDirectory: relative path refused", AddDllDirectory(L"rttest-dlls") == NULL);
    DLL_DIRECTORY_COOKIE c = AddDllDirectory(dir);
    CHECK("AddDllDirectory", c != NULL);
    HMODULE m = LoadLibraryW(L"dirtest1.dll");
    CHECK("found in the added directory", m != NULL && GetProcAddress(m, "testdll_add") != NULL);
    CHECK("RemoveDllDirectory", RemoveDllDirectory(c));
    CHECK("RemoveDllDirectory twice fails", !RemoveDllDirectory(c));
    CHECK("not found after removing", LoadLibraryW(L"dirtest2.dll") == NULL);

    CHECK("SetDllDirectoryW", SetDllDirectoryW(dir));
    CHECK("found in SetDllDirectory's directory", LoadLibraryW(L"dirtest3.dll") != NULL);
    CHECK("SetDllDirectoryW(NULL)", SetDllDirectoryW(NULL));
    CHECK("not found after resetting", LoadLibraryW(L"dirtest4.dll") == NULL);
}

int main(void)
{
    complex_math();
    conio();
    dll_directories();
    printf("rttest: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
