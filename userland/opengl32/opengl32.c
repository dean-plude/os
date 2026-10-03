/*
 * opengl32.dll — OpenGL, NovaOS's own front.
 *
 * On Windows, opengl32.dll is the system's and the display driver brings
 * the OpenGL implementation.  On NovaOS the implementations are Mesa's,
 * from the App Store, each a whole opengl32 of its own under another name:
 *
 *   opengl32_virgl.dll  Mesa's virgl ("Venus"): OpenGL on the host's GPU
 *                       through a 3D virtio-gpu (QEMU's virtio-vga-gl)
 *   opengl32_mesa.dll   Mesa's llvmpipe ("Mesa 3D"): OpenGL on the CPU
 *
 * When the process starts, this one picks virgl if the machine has a
 * virtio GPU with virgl's capability set and virgl is installed, and
 * llvmpipe otherwise, and every export (OpenGL 1.1 and wgl*) jumps to the
 * one picked.  GALLIUM_DRIVER chooses as in Mesa: "virgl" only virgl,
 * "llvmpipe" or "softpipe" only Mesa 3D.  With neither installed every call
 * fails (returns 0), as on a Windows machine without an OpenGL driver.
 */
#include <windows.h>

typedef LONG_PTR (WINAPI *NtNovaGpuCtl_t)(INT_PTR h, ULONG op, ULONG_PTR arg, void *ptr);

/* ---- the exports ---------------------------------------------------------- */
/* p_NAME: where NAME jumps; until a driver is loaded (or without one),
 * n_NAME, which returns 0 (popping the arguments, for 32-bit stdcall) */
#ifdef _WIN64
#define GL(n, b) __asm__(".text\n.globl n_" #n "\nn_" #n ":\n\txorl %eax, %eax\n\tret\n");
#else
#define GL(n, b) __asm__(".text\n.globl _n_" #n "\n_n_" #n ":\n\txorl %eax, %eax\n\tret $" #b "\n");
#endif
#include "gl_exports.h"
#undef GL

#define GL(n, b) extern char n_##n[]; void *p_##n = n_##n;
#include "gl_exports.h"
#undef GL

#ifdef _WIN64
#define GL(n, b) __asm__(".text\n.globl t_" #n "\nt_" #n ":\n\tjmp *p_" #n "(%rip)\n" \
                         ".section .drectve,\"yn\"\n\t.ascii \" /EXPORT:" #n "=t_" #n "\"\n.text\n");
#else
#define GL(n, b) __asm__(".text\n.globl _t_" #n "\n_t_" #n ":\n\tjmp *_p_" #n "\n" \
                         ".section .drectve,\"yn\"\n\t.ascii \" /EXPORT:" #n "=_t_" #n "\"\n.text\n");
#endif
#include "gl_exports.h"
#undef GL

static const struct { const char *name; void **slot; } g_slots[] = {
#define GL(n, b) { #n, &p_##n },
#include "gl_exports.h"
#undef GL
};

/* ---- picking the driver --------------------------------------------------- */
static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

/* A virtio GPU with 3D and a virgl capability set (VIRGL2 or VIRGL) */
static int virgl_gpu(void)
{
    NtNovaGpuCtl_t ctl = (NtNovaGpuCtl_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtNovaGpuCtl");
    struct { UINT32 present, pad; UINT64 host_visible; } info;
    struct { UINT32 id, version, size, pad; UINT64 out; } cs = { 2, 0, 0, 0, 0 };
    if (!ctl || ctl(0, 0, 0, &info) || !info.present) return 0;
    if (ctl(0, 1, 0, &cs) >= 0) return 1;
    cs.id = 1;
    return ctl(0, 1, 0, &cs) >= 0;
}

static void load_driver(void)
{
    char want[32];
    DWORD n = GetEnvironmentVariableA("GALLIUM_DRIVER", want, sizeof(want));
    if (!n || n >= sizeof(want)) want[0] = 0;
    HMODULE m = NULL;
    if (streq(want, "virgl") || (!want[0] && virgl_gpu()))
        m = LoadLibraryW(L"opengl32_virgl.dll");
    if (!m && !streq(want, "virgl"))
        m = LoadLibraryW(L"opengl32_mesa.dll");
    if (!m) return;
    for (size_t i = 0; i < sizeof(g_slots) / sizeof(g_slots[0]); i++) {
        void *f = (void *)GetProcAddress(m, g_slots[i].name);
        if (f) *g_slots[i].slot = f;
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD why, LPVOID reserved)
{
    (void)inst; (void)reserved;
    if (why == DLL_PROCESS_ATTACH) load_driver();
    return TRUE;
}
