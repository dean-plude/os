## A GPU path in QEMU (Venus)

- **The virtio GPU driver does 3D.**  On a QEMU `virtio-vga-gl` or
  `virtio-gpu-gl-pci` that offers 3D, blobs and context types (QEMU 9.2 or
  newer with `venus=on,blob=on,hostmem=...`), `kernel/drivers/virtio_gpu.c`
  takes them, reads the capability sets and finds the card's host-visible
  memory (`[VGPU] QEMU virtio-vga: 3D, 3 capability set(s), 1024 MiB of
  host-visible memory at ...`).  It now keeps up to 42 commands in flight
  at once, each in its own slot, because fenced 3D commands finish out of
  order; the 2D path (monitors, hot-plug) goes through the same queue.
  On a card without 3D nothing changes.
- **Vulkan programs reach it through `NtNovaGpuCtl`** (system call
  0x263, `kernel/um/um_gpu.c`), NovaOS's own version of what Linux's
  virtio-gpu DRM interface gives Mesa.  A handle is a 3D context on a
  capability set.  It submits command streams with fences, creates blobs,
  and maps a mappable blob as a section of the card's host-visible memory
  that `NtMapViewOfSection` maps (its views go away with the process).  It
  also keeps timelines that the fences advance, which a process reads,
  writes and waits on.  Every structure uses 64-bit fields, so 32-bit
  programs pass the same ones.
- **Venus, Mesa's Vulkan driver for virtio-gpu, runs on it.**
  `tools/build_venus.py` fetches Mesa 26.2.4 and patches it
  (`third_party/mesa-venus`): a renderer back end for NovaOS
  (`vn_renderer_nova.c`), Windows window surfaces for Venus, and a present
  that waits for the frame.  It then builds `vulkan_virtio.dll`, 64- and
  32-bit, with MinGW-w64.  The CI publishes the result, `venus.7z`, beside
  `nova.iso` on the "latest" release, and the App Store lists it as
  **Venus** (Runtimes).  Installed next to Mesa 3D and DXVK, it runs
  Vulkan programs, and Direct3D 8-11 ones through DXVK, on the host's GPU:
  `D3D9 adapter  Virtio-GPU Venus (llvmpipe (LLVM 20.1.2, 256 bits))`
  where the host has only Mesa's lavapipe, as CI's runners do.  Frames
  come back through host-visible memory and are drawn with the GDI.
- **The Vulkan loader prefers a GPU.**  `vulkan-1.dll` tries the drivers
  registered under `HKLM\SOFTWARE\Khronos\Vulkan\Drivers` that draw on a
  GPU before the ones that draw on the CPU (lavapipe), as the Khronos
  loader orders its physical devices.  Venus declines when there is no 3D
  virtio-gpu, so the same installation falls back to lavapipe on any
  other display.
- **CI's graphics boot runs on it.**  Its first monitor is now a
  `virtio-vga-gl,venus=on,blob=on,hostmem=1G` (the second is still a
  `secondary-vga`).  QEMU 10.2.1 and virglrenderer 1.3.0 with Venus are
  built from source by `tools/ci/build-qemu-venus.sh` and cached, and run
  on Xvfb with an SDL OpenGL window.  Ubuntu 24.04's QEMU 8.2 has no Venus.
  `tools/novarun.py` runs the QEMU that `NOVARUN_QEMU` names and opens
  that window only for a 3D GPU.  `d3dtest` (17 tests, 64- and 32-bit)
  must report the Venus adapter.  The new `d3dtest fps 10` draws a Direct3D
  9 scene that keeps the rasterizer busy (64 blended quads over 640x480)
  on Venus and on lavapipe, each in a child process whose
  `VK_DRIVER_FILES` names the driver, and Venus must draw more frames per
  second.  Under TCG it draws 14 to 17 frames per second against
  lavapipe's 0.12 to 0.13, and the animated part of `d3dtest` 461 frames against
  62.
- Not yet: virgl, which would put OpenGL on the host's GPU (OpenGL stays
  on llvmpipe); showing Vulkan's frames on the virtio GPU directly
  instead of copying them through the GDI; placed memory maps
  (`VK_EXT_map_memory_placed`); and a test on a host with a real GPU
  (CI's is lavapipe).
