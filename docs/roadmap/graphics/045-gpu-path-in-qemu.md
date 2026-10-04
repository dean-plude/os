- ~~**A GPU path in QEMU**~~ Done (Phase 19.7): the virtio GPU driver does
  3D, and Mesa's Venus (the App Store's "Venus") runs Vulkan, and Direct3D
  through DXVK, on the host's GPU through QEMU's `virtio-vga-gl,venus=on`;
  `d3dtest` passes on it and draws faster than on lavapipe, and ANGLE's
  Direct3D 11 back end (Chromium's GPU process) starts on it
  (`d3dtest angle`; [Chromium's GPU process](HISTORY.md#chromiums-gpu-process-on-direct3d-11)).
  ~~virgl (OpenGL on the host's GPU)~~ Done: the same App Store entry
  brings Mesa's virgl, which NovaOS's own `opengl32.dll` picks on that
  GPU; `gltest` passes on it and `gltest fps` draws faster than on
  llvmpipe; see [virgl](HISTORY.md#opengl-on-the-hosts-gpu-virgl).
  Still to do: showing Vulkan's and OpenGL's frames on the virtio GPU
  directly instead of reading them back and copying them through the GDI.
