## Direct3D: DXVK on Mesa's Vulkan

Programs that draw with Direct3D 8, 9, 10 or 11 get it rendered on the
CPU: [DXVK](https://github.com/doitsujin/dxvk) translates Direct3D to
Vulkan, and Mesa's lavapipe runs the Vulkan on the CPU (llvmpipe again
underneath).  As with OpenGL, these are existing open-source DLLs shipped
as OS components, and the programs are unchanged.  WineD3D (Direct3D to
OpenGL) was the other candidate; its source was out of reach from the
build machine, and DXVK is the faster, more complete path anyway.

- **App Store**: **Mesa 3D** now also installs `vulkan_lvp.dll` and its
  manifest (`lvp_icd.x86_64.json`, `lvp_icd.x86.json`) and registers the
  manifests under `HKLM\SOFTWARE\Khronos\Vulkan\Drivers`, as a driver
  installer does.  The new **DXVK** (Runtimes) installs `d3d8`, `d3d9`,
  `d3d10core` and `d3d11` into `System32`/`SysWOW64`, and DXVK's `dxgi`
  as `dxgi_dxvk.dll`.  The Store gained `.tar.gz` downloads (7-Zip takes
  the `.gz` layer off first), `path>name` renames, and the kernel helper
  `um_registry_set_dword` for installers' registrations.
- **`vulkan-1.dll`, NovaOS's own** (`userland/vulkan-1/`): finds the
  driver from that registry key (or `VK_DRIVER_FILES`/`VK_ICD_FILENAMES`),
  loads the first manifest's library that suits the process (64- or
  32-bit), negotiates the driver interface, and hands programs the
  driver's commands directly.  It exports the Khronos loader's 246 names;
  most are a jump through a pointer filled when the program creates its
  instance.  There are no layers and one driver at a time.  Mesa's Zink
  now finds it too, declines the CPU device, and OpenGL stays on llvmpipe.
- **`dxgi.dll`, NovaOS's own** (`userland/dxgi/`): a factory with no
  adapters, so programs that import DXGI start; with DXVK installed, it
  hands out DXVK's factory instead.  A Khronos `vulkan-1.dll` that a
  program carries beside its `.exe` loads `System32\dxgi.dll` and needs a
  factory before it reads its drivers; it keeps getting NovaOS's, since
  DXVK's would start Vulkan again and deadlock on DXVK's own lock.
- **`cfgmgr32.dll`** with empty device lists, for the loader and DXVK.
- **Frames on screen**: lavapipe presents with `StretchBlt` from a DIB
  section to the window's DC.  `BitBlt` and `StretchBlt` now copy rows
  for unscaled 32-bit copies and flush window DCs to the screen through
  `NovaFlushDC`, which skips a DC between `BeginPaint` and `EndPaint`
  (`EndPaint` presents).  The 64-bit D3D9 test went from 5 to 12 frames
  in three seconds at 320x240.
- **32-bit heap blocks are 16-byte aligned**, as on Windows: lavapipe's
  generated code reads them with `movdqa`.
- Also `AllocateLocallyUniqueId`, `EnumDisplayDevicesA`, the display
  configuration queries (`QueryDisplayConfig` and friends, answering "not
  supported"), `__C_specific_handler` from `kernel32` on x64, and the
  code bytes at a fault in JIT code in the serial log.
- Tested in QEMU with `tools/d3dtest/d3dtest.c` built with MinGW, 64-bit
  and 32-bit, after installing Mesa 3D and DXVK from the App Store: the
  D3D9 adapter (llvmpipe), a device, a fixed-function triangle read back
  through `GetRenderTargetData`, D3D11 at feature level 11_0 through a
  DXGI factory and adapter, a cleared render target read back through a
  staging texture, then animated frames with `Present` in a window
  (17/17 checks; D3D11 about 9 frames per second in a 320x240 window
  under QEMU without KVM).  OpenGL's `tools/gltest` still passes 14/14.
