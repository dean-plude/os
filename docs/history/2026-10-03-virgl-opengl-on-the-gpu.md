## OpenGL on the host's GPU (virgl)

- **OpenGL programs now draw on the host's GPU** when NovaOS runs in QEMU
  with a 3D virtio-gpu (`virtio-vga-gl`), as Vulkan and Direct3D programs
  have since Venus: Mesa's virgl encodes their OpenGL calls and QEMU's
  virglrenderer runs them on the host's OpenGL.  `gltest` reports
  `GL_RENDERER virgl (...)` and OpenGL 4.5, 64- and 32-bit.
- **The App Store's "Venus" brings it.**  `tools/build_venus.py` now builds
  Mesa 26.2.4's WGL `opengl32.dll` with the virgl driver beside Venus, from
  the same patched tree, and `venus.7z` carries it as
  `opengl32_virgl.dll` and `libgallium_virgl.dll`, 64- and 32-bit.  Its
  back end, `third_party/mesa-venus/virgl_nova_winsys.c`, is NovaOS's
  version of the winsys Linux's DRM interface gives virgl: a 3D context on
  the VIRGL2 capability set, classic 3D resources whose guest pages the
  kernel gives them, mappable host blobs for persistent and coherent
  buffers, and command streams and transfers fenced on one timeline in
  submission order, which tells when a resource is idle.  Frames reach the
  window through the GDI: the front buffer is read back into its pages and
  copied to the WGL display target, as Mesa's vtest does.
- **NovaOS has its own `opengl32.dll`.**  Like Windows', it is the
  system's, and the OpenGL implementation is the driver's: when a process
  starts it loads `opengl32_virgl.dll` if the machine has a virtio GPU with
  virgl's capability set and Venus is installed, and Mesa 3D's llvmpipe
  (installed as `opengl32_mesa.dll` now) otherwise, and its OpenGL 1.1 and
  `wgl*` exports jump to the one it loaded.  `GALLIUM_DRIVER=virgl` or
  `GALLIUM_DRIVER=llvmpipe` picks one, as in Mesa.
- **The kernel side** (`kernel/drivers/virtio_gpu.c`, `NtNovaGpuCtl` in
  `kernel/um/um_gpu.c`): two new operations create a classic 3D resource
  (`RESOURCE_CREATE_3D`, its guest pages attached with `ATTACH_BACKING` and
  the resource attached to the context) and transfer a box between it and
  its pages (`TRANSFER_TO_HOST_3D` / `TRANSFER_FROM_HOST_3D`, fenced on a
  timeline).  The resource's pages map into the program as a section, like
  a blob's host memory; they are freed only once the card has answered the
  resource's `RESOURCE_UNREF`.  Blobs are now attached to their context,
  as Linux does, because virgl's command streams name them.
- msvcrt has `qsort_s` (Mesa calls it).
- **Tests.**  The graphics suite runs `gltest` on virgl and on llvmpipe,
  64- and 32-bit, and the new `gltest fps 10` draws an OpenGL scene that
  keeps the rasterizer busy (64 blended quads over 640x480) for 10 s on
  virgl and 10 s on llvmpipe, each in a child process whose
  `GALLIUM_DRIVER` names the driver; virgl must draw more frames per
  second.  Under TCG it draws 12.6 to 15.9 frames per second against
  llvmpipe's 0.24 (52 to 65 times as many).
  CI's runner has no GPU: there virglrenderer draws with the host's
  llvmpipe, natively instead of inside NovaOS.
- Not yet: showing OpenGL's frames on the virtio GPU directly (they are
  read back every frame and drawn with the GDI), and virgl on a QEMU
  without blobs and context types (NovaOS's 3D path needs both).
