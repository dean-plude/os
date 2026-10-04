/*
 * avrt.dll — the Multimedia Class Scheduler (MMCSS) client
 *
 * A program's audio thread (or a game's) registers for a task, "Pro
 * Audio", "Audio", "Games"..., and runs at a real-time priority without
 * SeIncreaseBasePriorityPrivilege, above every busy or boosted program
 * thread, as MMCSS's service lifts it on Windows.  The kernel does the
 * work (NovaOS's NtSetInformationThread class ThreadNovaMmcss,
 * kernel/um/um_thread.c, and kernel/ke/scheduler.h's PRIO_MMCSS):
 *
 *   audio tasks (Pro Audio, Audio, Capture, Playback, Low Latency) at
 *   AVRT_PRIORITY_NORMAL and above       18  (Windows: 20-26)
 *   audio tasks at LOW and VERYLOW,
 *   every other task (Games, Distribution, Window Manager, ...)
 *                                        16  (Windows: 16-22)
 *
 * 18 is above the desktop (17) and below the kernel's device polling and
 * sound mixer (19); 16 below the desktop.  As on Windows, a registered
 * thread gets at most 80% of a processor (SystemResponsiveness 20), then
 * runs at its own priority until the next 100 ms period.  Reverting puts
 * it back at the priority SetThreadPriority gave it.
 */

#include <windows.h>
#include <winternl.h>

#define EXPORT __declspec(dllexport)

#define ThreadNovaMmcss        0x4E4D       /* (kernel/um/um_internal.h UM_THREAD_MMCSS) */
#define PRIO_MMCSS             16
#define PRIO_MMCSS_AUDIO       18
#define TASK_HANDLE(audio)     ((HANDLE)(ULONG_PTR)(0x41560000 | ((audio) ? 2 : 1)))
#define IS_TASK_HANDLE(h)      (((ULONG_PTR)(h) & ~(ULONG_PTR)3) == 0x41560000 && ((ULONG_PTR)(h) & 3))
#define TASK_AUDIO(h)          (((ULONG_PTR)(h) & 2) != 0)

#ifndef ERROR_INVALID_TASK_NAME
#define ERROR_INVALID_TASK_NAME 1550
#endif

/* The tasks Windows lists under HKLM\SOFTWARE\Microsoft\Windows NT\
 * CurrentVersion\Multimedia\SystemProfile\Tasks */
static const struct { const WCHAR *name; BOOL audio; } k_tasks[] = {
    { L"Audio", TRUE }, { L"Capture", TRUE }, { L"Distribution", FALSE }, { L"DisplayPostProcessing", FALSE },
    { L"Games", FALSE }, { L"Low Latency", TRUE }, { L"Playback", TRUE }, { L"Pro Audio", TRUE },
    { L"Window Manager", FALSE },
};

static LONG g_index;

/* The task's kind (1 audio, 0 other) or -1: no such task */
static int task_kind(LPCWSTR task)
{
    for (int i = 0; i < (int)(sizeof(k_tasks) / sizeof(k_tasks[0])); i++)
        if (!lstrcmpiW(task, k_tasks[i].name)) return k_tasks[i].audio;
    return -1;
}

static BOOL set_mm(ULONG prio)
{
    NTSTATUS st = NtSetInformationThread(GetCurrentThread(), ThreadNovaMmcss, &prio, sizeof(prio));
    if (st < 0) { SetLastError(RtlNtStatusToDosError(st)); return FALSE; }
    return TRUE;
}

static HANDLE characteristics(int kind, LPDWORD index)
{
    if (kind < 0) { SetLastError(ERROR_INVALID_TASK_NAME); return 0; }
    if (!set_mm(kind ? PRIO_MMCSS_AUDIO : PRIO_MMCSS)) return 0;
    if (!*index) *index = (DWORD)InterlockedIncrement(&g_index);
    return TASK_HANDLE(kind);
}

static int task_kind_a(LPCSTR task)
{
    WCHAR w[64];
    if (!MultiByteToWideChar(CP_ACP, 0, task, -1, w, 64)) return -1;
    return task_kind(w);
}

EXPORT HANDLE WINAPI AvSetMmThreadCharacteristicsW(LPCWSTR task, LPDWORD index)
{
    if (!task || !index) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    return characteristics(task_kind(task), index);
}
EXPORT HANDLE WINAPI AvSetMmThreadCharacteristicsA(LPCSTR task, LPDWORD index)
{
    if (!task || !index) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    return characteristics(task_kind_a(task), index);
}
/* The higher of two tasks */
static int max_kind(int a, int b)
{
    return a < 0 || b < 0 ? -1 : a > b ? a : b;
}
EXPORT HANDLE WINAPI AvSetMmMaxThreadCharacteristicsW(LPCWSTR a, LPCWSTR b, LPDWORD index)
{
    if (!a || !b || !index) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    return characteristics(max_kind(task_kind(a), task_kind(b)), index);
}
EXPORT HANDLE WINAPI AvSetMmMaxThreadCharacteristicsA(LPCSTR a, LPCSTR b, LPDWORD index)
{
    if (!a || !b || !index) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    return characteristics(max_kind(task_kind_a(a), task_kind_a(b)), index);
}
EXPORT BOOL WINAPI AvRevertMmThreadCharacteristics(HANDLE h)
{
    if (!IS_TASK_HANDLE(h)) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    return set_mm(0);
}
/* AVRT_PRIORITY_VERYLOW (-2) .. CRITICAL (2): an audio task below NORMAL
 * drops to 16 */
EXPORT BOOL WINAPI AvSetMmThreadPriority(HANDLE h, int prio)
{
    if (!IS_TASK_HANDLE(h)) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (prio < -2 || prio > 2) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    return set_mm(TASK_AUDIO(h) && prio >= 0 ? PRIO_MMCSS_AUDIO : PRIO_MMCSS);
}
EXPORT BOOL WINAPI AvQuerySystemResponsiveness(HANDLE h, PULONG v)
{
    if (!IS_TASK_HANDLE(h) || !v) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    *v = 20;
    return TRUE;
}
