/* nova_main.c — small NovaOS hooks the NetSurf framebuffer frontend calls */
#include <windows.h>

/* The browser window's size when the options don't give one: most of the
 * desktop, leaving room for the title bar and the taskbar. */
void nova_default_window_size(int *width, int *height)
{
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    if (sw <= 0 || sh <= 0) return;
    int w = sw - 80, h = sh - 140;
    if (w > 1280) w = 1280;
    if (h > 900) h = 900;
    if (w < 640) w = 640;
    if (h < 400) h = 400;
    *width = w;
    *height = h;
}
