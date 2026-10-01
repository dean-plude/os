/*
 * vulkan-1.dll — the Vulkan loader, NovaOS's own.
 *
 * Programs (and DXVK) load vulkan-1.dll and reach a Vulkan driver through
 * it.  NovaOS's loader is a small one: it finds the installable client
 * driver (ICD) the way the Khronos loader does on Windows, from
 * HKLM\SOFTWARE\Khronos\Vulkan\Drivers (each value a driver's JSON
 * manifest, enabled when its DWORD is 0; VK_DRIVER_FILES or
 * VK_ICD_FILENAMES override), loads the first that works, and hands its
 * commands to the program directly.  There are no layers, and one driver
 * at a time (Mesa's lavapipe, from the App Store's Mesa 3D).
 *
 * The exports are the Khronos loader's (vk_exports.h): each is a jump
 * through a pointer filled from the driver when the program creates its
 * instance.
 */
#include <windows.h>
#include <winreg.h>

#define VKAPI __declspec(dllexport)

typedef int VkResult;
#define VK_SUCCESS                      0
#define VK_ERROR_INITIALIZATION_FAILED  (-3)
#define VK_ERROR_LAYER_NOT_PRESENT      (-6)
#define VK_ERROR_INCOMPATIBLE_DRIVER    (-9)

typedef void (WINAPI *PFN_vkVoidFunction)(void);
typedef PFN_vkVoidFunction (WINAPI *PFN_gipa)(void *instance, const char *name);
typedef VkResult (WINAPI *PFN_negotiate)(UINT32 *version);
typedef VkResult (WINAPI *PFN_create)(const void *info, const void *alloc, void **instance);
typedef void (WINAPI *PFN_destroy)(void *instance, const void *alloc);
typedef VkResult (WINAPI *PFN_enum_ext)(const char *layer, UINT32 *count, void *props);
typedef VkResult (WINAPI *PFN_version)(UINT32 *version);

typedef struct {                       /* VkInstanceCreateInfo */
    int          sType;
    const void  *pNext;
    UINT32       flags;
    const void  *pApplicationInfo;
    UINT32       enabledLayerCount;
    const char *const *ppEnabledLayerNames;
    UINT32       enabledExtensionCount;
    const char *const *ppEnabledExtensionNames;
} InstanceInfo;

static PFN_gipa  g_gipa;               /* the driver's vk_icdGetInstanceProcAddr */
static void     *g_instance;           /* the instance the exports were filled from */
static PFN_vkVoidFunction g_gdpa;      /* the driver's vkGetDeviceProcAddr */

/* ---- the exports that go straight to the driver ------------------------- */
#define VK(n) void *p_##n;
#include "vk_exports.h"
#undef VK

#ifdef _WIN64
#define VK(n) __asm__(".text\n.globl t_" #n "\nt_" #n ":\n\tjmp *p_" #n "(%rip)\n" \
                      ".section .drectve,\"yn\"\n\t.ascii \" /EXPORT:" #n "=t_" #n "\"\n.text\n");
#else
#define VK(n) __asm__(".text\n.globl _t_" #n "\n_t_" #n ":\n\tjmp *_p_" #n "\n" \
                      ".section .drectve,\"yn\"\n\t.ascii \" /EXPORT:" #n "=_t_" #n "\"\n.text\n");
#endif
#include "vk_exports.h"
#undef VK

static const struct { const char *name; void **slot; } g_slots[] = {
#define VK(n) { #n, &p_##n },
#include "vk_exports.h"
#undef VK
};

/* ---- finding the driver ------------------------------------------------- */
static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

/* The JSON string after "key": copied (escapes undone) into @out */
static int json_string(const char *j, const char *key, char *out, int size)
{
    for (const char *p = j; *p; p++) {
        if (*p != '"') continue;
        const char *k = key, *q = p + 1;
        while (*k && *q == *k) k++, q++;
        if (*k || *q != '"') continue;
        q++;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        if (*q++ != ':') continue;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        if (*q++ != '"') return 0;
        int n = 0;
        for (; *q && *q != '"' && n < size - 1; q++) {
            if (*q == '\\' && q[1]) q++;
            out[n++] = *q;
        }
        out[n] = 0;
        return n > 0;
    }
    return 0;
}

/* Load the driver a JSON manifest names; true if it is usable */
static int try_manifest(const char *path)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    char j[4096];
    DWORD got = 0;
    BOOL ok = ReadFile(h, j, sizeof(j) - 1, &got, NULL);
    CloseHandle(h);
    if (!ok) return 0;
    j[got] = 0;

    char lib[MAX_PATH], arch[8], full[2 * MAX_PATH];
    if (!json_string(j, "library_path", lib, sizeof(lib))) return 0;
    if (json_string(j, "library_arch", arch, sizeof(arch)) && !streq(arch, sizeof(void *) == 8 ? "64" : "32"))
        return 0;
    int slash = 0;
    for (const char *c = lib; *c; c++) if (*c == '\\' || *c == '/') slash = 1;
    int absolute = lib[0] == '\\' || lib[0] == '/' || (lib[0] && lib[1] == ':');
    if (slash && !absolute) {                         /* relative to the manifest's folder */
        int n = 0, cut = 0;
        for (; path[n] && n < MAX_PATH; n++) {
            full[n] = path[n];
            if (path[n] == '\\' || path[n] == '/') cut = n + 1;
        }
        n = cut;
        for (const char *c = lib; *c && n < (int)sizeof(full) - 1; c++) full[n++] = *c;
        full[n] = 0;
    } else {
        int n = 0;
        for (; lib[n]; n++) full[n] = lib[n];
        full[n] = 0;
    }

    HMODULE m = LoadLibraryA(full);
    if (!m) return 0;
    PFN_negotiate neg = (PFN_negotiate)GetProcAddress(m, "vk_icdNegotiateLoaderICDInterfaceVersion");
    PFN_gipa gipa = (PFN_gipa)GetProcAddress(m, "vk_icdGetInstanceProcAddr");
    UINT32 version = 5;                 /* the driver creates surfaces; any API version */
    if (!gipa || (neg && neg(&version) != VK_SUCCESS) || (neg && version < 3)) {
        FreeLibrary(m);
        return 0;
    }
    g_gipa = gipa;
    return 1;
}

static int try_list(char *list)
{
    for (char *p = list, *next; p && *p; p = next) {
        next = p;
        while (*next && *next != ';') next++;
        if (*next) *next++ = 0;
        else next = NULL;
        if (*p && try_manifest(p)) return 1;
    }
    return 0;
}

static BOOL CALLBACK find_driver(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    char buf[1024];
    if ((GetEnvironmentVariableA("VK_DRIVER_FILES", buf, sizeof(buf)) ||
         GetEnvironmentVariableA("VK_ICD_FILENAMES", buf, sizeof(buf))) && try_list(buf))
        return TRUE;
    HKEY k;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\Vulkan\\Drivers", 0, KEY_READ, &k)) return TRUE;
    for (DWORD i = 0; ; i++) {
        char name[MAX_PATH];
        DWORD n = sizeof(name), type = 0, val = 1, size = sizeof(val);
        if (RegEnumValueA(k, i, name, &n, NULL, &type, (BYTE *)&val, &size)) break;
        if (type == REG_DWORD && val == 0 && try_manifest(name)) break;
    }
    RegCloseKey(k);
    return TRUE;
}

static int driver(void)
{
    static INIT_ONCE once;
    InitOnceExecuteOnce(&once, find_driver, NULL, NULL);
    return g_gipa != NULL;
}

/* ---- the commands the loader itself answers ----------------------------- */
VKAPI PFN_vkVoidFunction WINAPI vkGetInstanceProcAddr(void *instance, const char *name);

VKAPI VkResult WINAPI vkCreateInstance(const InstanceInfo *info, const void *alloc, void **instance)
{
    if (!driver()) return VK_ERROR_INCOMPATIBLE_DRIVER;
    if (info && info->enabledLayerCount) return VK_ERROR_LAYER_NOT_PRESENT;
    PFN_create create = (PFN_create)g_gipa(NULL, "vkCreateInstance");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = create(info, alloc, instance);
    if (r == VK_SUCCESS && !g_instance) {
        /* The driver's commands work for any of its objects: fill the
         * exports once, from the first instance */
        for (UINT i = 0; i < sizeof(g_slots) / sizeof(g_slots[0]); i++)
            *g_slots[i].slot = (void *)g_gipa(*instance, g_slots[i].name);
        g_gdpa = g_gipa(*instance, "vkGetDeviceProcAddr");
        g_instance = *instance;
    }
    return r;
}

VKAPI void WINAPI vkDestroyInstance(void *instance, const void *alloc)
{
    if (!instance || !driver()) return;
    PFN_destroy destroy = (PFN_destroy)g_gipa(instance, "vkDestroyInstance");
    if (destroy) destroy(instance, alloc);
}

VKAPI VkResult WINAPI vkEnumerateInstanceExtensionProperties(const char *layer, UINT32 *count, void *props)
{
    if (layer) return VK_ERROR_LAYER_NOT_PRESENT;
    PFN_enum_ext e = driver() ? (PFN_enum_ext)g_gipa(NULL, "vkEnumerateInstanceExtensionProperties") : NULL;
    if (!e) { *count = 0; return VK_SUCCESS; }
    return e(layer, count, props);
}

VKAPI VkResult WINAPI vkEnumerateInstanceLayerProperties(UINT32 *count, void *props)
{
    (void)props;
    *count = 0;
    return VK_SUCCESS;
}

VKAPI VkResult WINAPI vkEnumerateDeviceLayerProperties(void *physical, UINT32 *count, void *props)
{
    (void)physical; (void)props;
    *count = 0;
    return VK_SUCCESS;
}

VKAPI VkResult WINAPI vkEnumerateInstanceVersion(UINT32 *version)
{
    PFN_version v = driver() ? (PFN_version)g_gipa(NULL, "vkEnumerateInstanceVersion") : NULL;
    if (v) return v(version);
    *version = 1u << 22;                                    /* Vulkan 1.0 */
    return VK_SUCCESS;
}

VKAPI PFN_vkVoidFunction WINAPI vkGetDeviceProcAddr(void *device, const char *name)
{
    if (!g_gdpa) return NULL;
    return ((PFN_vkVoidFunction (WINAPI *)(void *, const char *))g_gdpa)(device, name);
}

VKAPI PFN_vkVoidFunction WINAPI vkGetInstanceProcAddr(void *instance, const char *name)
{
    if (!name) return NULL;
    static const struct { const char *name; PFN_vkVoidFunction fn; } own[] = {
        { "vkGetInstanceProcAddr",                  (PFN_vkVoidFunction)vkGetInstanceProcAddr },
        { "vkCreateInstance",                       (PFN_vkVoidFunction)vkCreateInstance },
        { "vkDestroyInstance",                      (PFN_vkVoidFunction)vkDestroyInstance },
        { "vkEnumerateInstanceExtensionProperties", (PFN_vkVoidFunction)vkEnumerateInstanceExtensionProperties },
        { "vkEnumerateInstanceLayerProperties",     (PFN_vkVoidFunction)vkEnumerateInstanceLayerProperties },
        { "vkEnumerateDeviceLayerProperties",       (PFN_vkVoidFunction)vkEnumerateDeviceLayerProperties },
        { "vkEnumerateInstanceVersion",             (PFN_vkVoidFunction)vkEnumerateInstanceVersion },
    };
    for (UINT i = 0; i < sizeof(own) / sizeof(own[0]); i++)
        if (streq(name, own[i].name)) return own[i].fn;
    if (!driver()) return NULL;
    return g_gipa(instance, name);
}
