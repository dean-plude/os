/* crt0 — NovaOS C program startup, linked into every .exe */
__declspec(dllimport) int  __getmainargs(int *argc, char ***argv, char ***envp, int glob, void *si);
__declspec(dllimport) __declspec(noreturn) void exit(int code);
int main(int argc, char **argv, char **envp);

int _fltused = 0x9875;

/* std::type_info's vtable (every C++ RTTI/EH type descriptor points at it;
 * MSVC links it from the static CRT too).  type_info objects are static
 * data, so the deleting destructor has nothing to free. */
#ifdef _WIN64
static void *__cdecl type_info_delete(void *self, unsigned flags) { (void)flags; return self; }
#else                                           /* x86 virtual functions: thiscall */
static void *__thiscall type_info_delete(void *self, unsigned flags) { (void)flags; return self; }
#endif
void *const nova_type_info_vtable[1] __asm__("??_7type_info@@6B@") = { (void *)type_info_delete };

/* Static constructors (C++ globals, __attribute__((constructor))): the
 * compiler puts pointers to them in .CRT$XCU; the linker sorts the .CRT$
 * sections by name, so they land between these two markers. */
typedef void (__cdecl *_PVFV)(void);
#pragma section(".CRT$XCA", long, read)
#pragma section(".CRT$XCZ", long, read)
__declspec(allocate(".CRT$XCA")) const _PVFV __xc_a[] = { 0 };
__declspec(allocate(".CRT$XCZ")) const _PVFV __xc_z[] = { 0 };

static void run_initializers(void)
{
    const _PVFV *p = __xc_a, *end = __xc_z;
    __asm__("" : "+r"(p), "+r"(end));       /* distinct arrays: stop the optimizer reasoning */
    for (; p < end; p++)
        if (*p) (*p)();
}

void mainCRTStartup(void)
{
    int argc = 0;
    char **argv = 0, **envp = 0;
    __getmainargs(&argc, &argv, &envp, 0, 0);
    run_initializers();
    exit(main(argc, argv, envp));
}
