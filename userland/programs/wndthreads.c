/* wndthreads.exe — windows whose thread has ended, and destroying a parent
 * whose child windows belong to another thread.
 *
 * A thread's windows answer nothing once the thread has ended: a message
 * sent to one returns at once, and DestroyWindow on its parent returns.
 * VLC's Qt window thread used to wait for ever in DestroyWindow for the
 * child window VLC's video event thread had made (the event thread had
 * ended on that child's WM_DESTROY), so VLC never closed.
 *
 * A watchdog ends the program (exit code 3) if any step hangs, so the
 * test fails instead of freezing the Terminal. */
#include <stdio.h>
#include <windows.h>

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

static const char *g_step = "start";

static DWORD WINAPI watchdog(void *arg)
{
    (void)arg;
    Sleep(60000);
    printf("FAIL: hung at: %s\n", g_step);
    fflush(stdout);
    ExitProcess(3);
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_USER + 7) return 0x1234;
    return DefWindowProcA(h, m, wp, lp);
}

/* the child's thread ends on the child's WM_DESTROY, as VLC's event thread does */
static LRESULT CALLBACK quit_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_DESTROY) PostQuitMessage(0);
    return proc(h, m, wp, lp);
}

static HWND child(HWND parent, WNDPROC fn)
{
    HWND h = CreateWindowExA(0, "wndthreads", "", WS_CHILD | WS_VISIBLE, 0, 0, 50, 50, parent, NULL, NULL, NULL);
    if (h && fn) SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)fn);
    return h;
}

typedef struct { HWND parent, made, grandchild; HANDLE ready; int pump; } Job;

/* Make a child of @parent (and a grandchild under it); pump messages
 * until WM_QUIT if @pump, else end at once */
static DWORD WINAPI maker(void *arg)
{
    Job *j = arg;
    j->made = child(j->parent, j->pump ? quit_proc : NULL);
    j->grandchild = j->made ? child(j->made, NULL) : NULL;
    SetEvent(j->ready);
    if (j->pump) {
        MSG m;
        while (GetMessageA(&m, NULL, 0, 0) > 0) DispatchMessageA(&m);
    }
    return 0;
}

/* Wait for @h while answering messages: making a child window sends its
 * parent (this thread's window) WM_PARENTNOTIFY */
static DWORD wait_pumping(HANDLE h, DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    for (;;) {
        DWORD now = GetTickCount();
        if ((LONG)(end - now) <= 0) return WAIT_TIMEOUT;
        DWORD r = MsgWaitForMultipleObjects(1, &h, FALSE, end - now, QS_ALLINPUT);
        if (r != WAIT_OBJECT_0 + 1) return r;
        MSG m;
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&m);
    }
}

static HWND start(Job *j, HWND parent, int pump, HANDLE *thread)
{
    j->parent = parent;
    j->pump = pump;
    j->ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    *thread = CreateThread(NULL, 0, maker, j, 0, NULL);
    wait_pumping(j->ready, 10000);
    CloseHandle(j->ready);
    return j->made;
}

static HWND top(void)
{
    return CreateWindowExA(0, "wndthreads", "wndthreads", WS_OVERLAPPEDWINDOW, 40, 40, 200, 150, NULL, NULL, NULL, NULL);
}

int main(void)
{
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = proc;
    wc.lpszClassName = "wndthreads";
    CHECK("RegisterClass", RegisterClassA(&wc) != 0);

    /* a live thread's window answers a message sent from here */
    g_step = "send to a live thread's window";
    HWND p = top();
    Job j = { 0 };
    HANDLE t;
    HWND c = start(&j, p, 1, &t);
    CHECK("child made by another thread", c != NULL);
    CHECK("sent message answered", SendMessageA(c, WM_USER + 7, 0, 0) == 0x1234);

    /* destroying the parent destroys the other thread's child, whose
     * thread ends on its WM_DESTROY while its grandchild is still to go */
    g_step = "DestroyWindow, child's thread ends on WM_DESTROY";
    CHECK("DestroyWindow", DestroyWindow(p));
    CHECK("child gone", !IsWindow(c));
    CHECK("grandchild gone", !IsWindow(j.grandchild));
    CHECK("child's thread ended", wait_pumping(t, 10000) == WAIT_OBJECT_0);
    CloseHandle(t);

    /* a window whose thread has ended answers nothing, at once */
    g_step = "send to an ended thread's window";
    p = top();
    Job k = { 0 };
    c = start(&k, p, 0, &t);
    CHECK("child made by another thread", c != NULL);
    CHECK("its thread ended", wait_pumping(t, 10000) == WAIT_OBJECT_0);
    CloseHandle(t);
    DWORD t0 = GetTickCount();
    CHECK("sent message gets no answer", SendMessageA(c, WM_USER + 7, 0, 0) == 0);
    CHECK("and returns at once", GetTickCount() - t0 < 2000);

    /* and its parent can still be destroyed */
    g_step = "DestroyWindow, child's thread ended";
    CHECK("DestroyWindow", DestroyWindow(p));
    CHECK("parent gone", !IsWindow(p));
    CHECK("child gone", !IsWindow(c));
    CHECK("grandchild gone", !IsWindow(k.grandchild));

    printf("wndthreads: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
