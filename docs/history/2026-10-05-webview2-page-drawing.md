## WebView2 draws its page

With the browser's window inside the host's, WebView2 loaded its page and
ran its scripts, but the window stayed black.  On a PC without a GPU,
Chromium's GPU process draws with its software compositor, and the
current one (Chromium 154, under the WebView2 runtime) does not paint the
window with GDI: it draws each frame into a mapped Direct3D 11 staging
texture on WARP, copies the changed part into a DXGI 1.2 swap chain made
for DirectComposition and presents it, and a DirectComposition visual
shows the swap chain in a window the GPU process makes and the browser
process parents in its own.  NovaOS gave no DXGI factory interface past
`IDXGIFactory1`, no WARP device and no DirectComposition, so the GPU
process stopped on its first call (`CreateDXGIFactory1` for an
`IDXGIFactory2`) and the browser gave up after three tries.  Now the
host's window shows the page.  Nothing in the runtime is changed.

- **dxgi.dll**: without DXVK its factory is `IDXGIFactory7` and lists
  one adapter, as Windows does on a PC without a display driver: the
  Microsoft Basic Render Driver (VendorId `0x1414`, DeviceId `0x8c`, a
  software adapter with no outputs; `EnumWarpAdapter`,
  `EnumAdapterByLuid`, video memory queries, the driver version
  Chromium reads).  It makes swap chains on the software device, for a
  window (presented with GDI) and for composition (`IDXGISwapChain1`,
  `Present1` with dirty rectangles).  The Khronos Vulkan loader skips
  software adapters, so it is unaffected.
- **d3d11.dll**: `D3D11CreateDevice` with `D3D_DRIVER_TYPE_WARP` (or the
  Basic Render Driver adapter) gives a software device of feature level
  9_1 whose textures live in memory: created, mapped, updated and copied
  by the CPU.  It does not rasterize, and a request that needs feature
  level 9_3 or higher (ANGLE's) is refused, so ANGLE reports no renderer
  and Chromium takes its software path.  64-bit only.  With DXVK
  installed everything still goes to DXVK.
- **dcomp.dll** (new): a DirectComposition device, targets on windows and
  visuals with offsets and children; a visual whose content is a swap
  chain draws each present into its target's window.  Only
  `DCompositionCreateDevice` is exported, so Chromium keeps its hardware
  DirectComposition path (which needs a GPU) off.
- **Windows of other processes**: the window the GPU process composites
  into is a child of a hidden window of its own until the browser calls
  `SetParent` on it.  NovaOS now tells the GPU process, whose child window
  then becomes a desktop window of its own that the desktop keeps in the
  browser's window (it stays `WS_CHILD`, and `GetParent` gives the
  browser's window).

The new self-test `dcomptest` (64- and 32-bit) does what Chromium does:
the adapter, a WARP device, a composition swap chain shown by a visual,
dirty-rectangle presents read back from the window, a swap chain on a
window, and the GPU process's window parented by another process and
drawn into.
