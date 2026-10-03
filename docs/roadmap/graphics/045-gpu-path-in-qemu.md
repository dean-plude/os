- ~~**A GPU path in QEMU**~~ Done (Phase 19.7): the virtio GPU driver does
  3D, and Mesa's Venus (the App Store's "Venus") runs Vulkan, and Direct3D
  through DXVK, on the host's GPU through QEMU's `virtio-vga-gl,venus=on`;
  `d3dtest` passes on it and draws faster than on lavapipe.  Still to do:
  virgl (OpenGL on the host's GPU; OpenGL stays on llvmpipe), and showing
  Vulkan's frames on the virtio GPU directly instead of copying them
  through the GDI.
