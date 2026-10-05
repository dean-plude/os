## Direct3D 11 textures and fences shared between devices, on lavapipe

GOG GALAXY's client draws its window with Qt WebEngine, whose Chromium
renders each web page with Direct3D 11 (ANGLE on DXVK) into a texture it
shares, by handle, with Qt's own Direct3D 11 device.  On the App Store's
Mesa 3D, DXVK runs on lavapipe, which has no Windows build of the Vulkan
extensions DXVK shares textures with, so Chromium's compositor lost its
context and the client stopped.  NovaOS's Vulkan loader now provides them,
and the client gets past that point
([compatibility.md](../compatibility.md)).

- **Shared memory** (`userland/vulkan-1/shared.c`): for a driver that can
  use memory it is handed (`VK_EXT_external_memory_host`) but lacks
  `VK_KHR_external_memory_win32`, `vulkan-1.dll` adds the extension.
  Memory allocated for export is a section (`CreateFileMapping`) mapped
  into the process and given to the driver as host memory; its handle is
  a new handle to the section, and importing it maps the same pages, in
  the same process or another one the handle reached.  Images, buffers,
  format queries and device creation are answered as a driver with the
  extension would.  Only NT handles are provided, not the older global
  (KMT) handles.
- **Shared fences**: `VK_KHR_external_semaphore_win32` the same way, for
  timeline semaphores (`ID3D11Fence` with `D3D11_FENCE_FLAG_SHARED`, which
  Chromium creates for Qt): the value lives in a section, and a thread of
  the loader copies a new value between each device's semaphore and the
  section, so a wait on one device ends when another signals.  DXVK used
  to call a missing `vkGetSemaphoreWin32HandleKHR` there and crash.
- **A shared texture's description**: DXVK keeps it with the handle
  through `DeviceIoControl` (Proton's `sharedgpures.sys` requests).  For a
  section handle the kernel now stores and returns it
  (`kernel/um/um_thread.c`), and kernel32's `DeviceIoControl` passes
  video-device requests to `NtDeviceIoControlFile`.
- **Module file names past the 64th module**: `GetModuleFileName` and
  `EnumProcessModules` only looked at a process's first 64 modules.  The
  client has 89, and MFC's `DllMain` throws when `GetModuleFileName` does
  not know `mfc140u.dll`, so since a refused `DllMain` ends the process
  (as on Windows) the client did not start at all.
- **Tests**: `d3dtest shared` (graphics suite, on lavapipe, 64- and
  32-bit, 13 checks) shares a texture between two devices both ways and
  with a child process, and a fence signalled on either device;
  `dlltest` (core suite) loads more than 64 modules and checks every
  one's file name.
- **Where the client stops now**: Chromium's ANGLE compiles its shaders
  with `d3dcompiler_47.dll`, and NovaOS's has no HLSL compiler, so
  Chromium's compositor still loses its context and the client ends on a
  `CHECK` in `qt6webenginecore.dll`.
