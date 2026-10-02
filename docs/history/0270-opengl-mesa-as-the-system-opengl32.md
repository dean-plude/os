## OpenGL: Mesa as the system `opengl32.dll`

Programs that draw with OpenGL get OpenGL 4.5 rendered on the CPU by
Mesa's llvmpipe, the same approach Wine takes: an existing open-source
DLL ships as an OS component, and the programs are not changed.  The
App Store's **Mesa 3D** (Runtimes) installs the
[mesa-dist-win](https://github.com/pal1000/mesa-dist-win) 24.2.4 build:
7-Zip unpacks just `opengl32.dll`, `libgallium_wgl.dll` and
`libglapi.dll`, 64-bit into `C:\Windows\System32` and 32-bit into
`C:\Windows\SysWOW64`, so every program that imports `opengl32.dll`
finds them.

- **WGL in gdi32** (`userland/gdi32/wgl.c`): `ChoosePixelFormat`,
  `DescribePixelFormat`, `GetPixelFormat`, `SetPixelFormat` and
  `SwapBuffers` load `opengl32.dll` on first use and call its `wgl*`
  functions, as on Windows.  GDI remembers each window's pixel format
  (set once), and a per-thread guard answers Mesa's own calls back into
  these from what GDI knows.
- **Presenting frames**: Mesa shows each frame with `StretchDIBits`.  An
  unscaled 32-bit blit now copies rows directly, and user32's
  `NovaFlushDC` puts the change on screen, since GL programs draw
  through a DC they keep for the window's life instead of painting in
  `WM_PAINT`.
- **x86 `SLIST_HEADER`** is 8 bytes, as on Windows (it was the 16-byte
  x64 layout, which 32-bit Mesa's aligned stores faulted on).
- Also `RtlGetLastNtStatus`, `HeapWalk`/`HeapLock`/`HeapUnlock` and
  `EnumDisplaySettingsA`.
- Tested in QEMU with `tools/gltest/gltest.c` built with MinGW, 64-bit
  and 32-bit, after installing Mesa from the App Store: vendor, renderer
  and version strings, clearing, immediate-mode drawing, a GLSL shader,
  pixel read-back, then animated frames with `SwapBuffers` (14/14 checks,
  about 48 frames per second in a 320x240 window under QEMU without KVM).  Mesa
  also tries its Zink (Vulkan) and D3D12 drivers first and logs that
  `vulkan-1.dll` and `d3d12.dll` are missing; it then uses llvmpipe.
- Next: Direct3D on top of this (WineD3D to OpenGL, or DXVK on Mesa's
  lavapipe Vulkan).
