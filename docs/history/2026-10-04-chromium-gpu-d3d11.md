## Chromium's GPU process on Direct3D 11

Steam's browser (`steamwebhelper.exe`, Chromium 126) started its GPU
process, which then logged "Initialization of all EGL display types
failed" and, after three tries, Chromium fell back to drawing in software.
Edge's WebView2 runtime and Qt WebEngine (GOG GALAXY) run the same GPU
code.  Chromium draws through ANGLE, which turns OpenGL ES into
Direct3D 11.

The cause was in `user32`, not in Direct3D.  Chromium hands ANGLE the
screen's DC, `GetDC(NULL)`, as its EGL display, and ANGLE takes a DC as a
display only when `WindowFromDC` finds the window it belongs to.  On
Windows the screen's DC belongs to the desktop window; NovaOS returned no
window, so `eglGetPlatformDisplayEXT` gave `EGL_NO_DISPLAY` for every
display type Chromium tried, with or without a Direct3D driver installed.
`WindowFromDC(GetDC(NULL))` now returns `GetDesktopWindow()`, as on
Windows (`EnumDisplayMonitors` and `ScrollDC` keep treating the screen's
DC as before).

With the App Store's Mesa 3D and DXVK (or Venus and DXVK) installed, ANGLE
(checked with Firefox's build of it) now creates its Direct3D 11 display
on the DXVK adapter (`ANGLE (Mesa, llvmpipe ...) Direct3D11 vs_5_0
ps_5_0`), draws into a pbuffer and presents to a window.  Without a
Direct3D driver, ANGLE now reports "No available renderers", as on a PC
whose adapter has no Direct3D 11 driver, and Chromium draws in software.
NovaOS has no WARP adapter of its own: `D3D_DRIVER_TYPE_WARP` reaches DXVK
like every other driver type.

What ANGLE asks of Direct3D 11 on the way, all of which DXVK answers:
the first DXGI adapter whose vendor is not Microsoft's, `D3D11CreateDevice`
with feature levels 11.1 down to 9.3 (again without 11.1 on
`E_INVALIDARG`), `IDXGIDevice2` (DXGI 1.2), `ID3D11DeviceContext1` and
`ID3D11DeviceContext3`, the adapter's description, parent factory and
driver version (`CheckInterfaceSupport(IDXGIDevice)`), the D3D11 options
and format queries, and a DXGI 1.2 swap chain
(`CreateSwapChainForHwnd`, sequential).  Lavapipe has no
`VK_KHR_external_memory_win32`, so DXVK cannot share textures between
devices: ANGLE logs that it could not get a pbuffer's shared handle and
carries on.

`d3dtest angle` (graphics suite, `215-d3dtest-angle`, 64- and 32-bit)
brings Direct3D 11 up in that order, on Venus in CI.
