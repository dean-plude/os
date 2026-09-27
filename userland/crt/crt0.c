/* crt0 — NovaOS C program startup, linked into every .exe */
__declspec(dllimport) int  __getmainargs(int *argc, char ***argv, char ***envp, int glob, void *si);
__declspec(dllimport) __declspec(noreturn) void exit(int code);
int main(int argc, char **argv, char **envp);

int _fltused = 0x9875;

void mainCRTStartup(void)
{
    int argc = 0;
    char **argv = 0, **envp = 0;
    __getmainargs(&argc, &argv, &envp, 0, 0);
    exit(main(argc, argv, envp));
}
