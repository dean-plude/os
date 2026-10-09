# App, graphics, kernel, storage and hardware work

This is a staged implementation backlog, not a declaration that these
features work. Use current corpus results and source fragments when
selecting the next change; generated roadmap regions can lag their sources.

| Area | Next concrete increment | Completion evidence |
|---|---|---|
| App coverage | Reproduce a strict WebView2 or Firefox failure from a completed corpus, then fix the demonstrated API gap; add one additional App Store application after the baseline is stable. | End-to-end install, representative user action, saved output or verified page contents, clean exit; a new per-app corpus file and honest compatibility fragment. |
| GPU | First validate WebView2's ANGLE/DXVK child-window presentation on Venus; then prototype direct virtio GPU presentation to avoid CPU readback and GDI copies. | Correct frames, resize, cross-process parenting and teardown; x64/x86 where applicable; frame-time and copied-byte measurements against the existing fallback. |
| Kernel/API | Complete console screen-state queries and cell operations after the cursor controls in this change. | Shared screen state across handles/processes; coordinate clipping and error tests. |
| Storage | Verify and complete NTFS hard links and persistence, using a disposable disk image; test interrupted writes and low-space behavior. | Two names share file identity/data; removing one leaves the other readable; correct behavior after reboot and read-only mounting of unclean volumes. |
| Hardware | Run the existing reference ThinkPad T14 Gen 4 boot/device checklist; use observed PCI IDs and logs to pick the first missing driver. | Real-machine boot log and device inventory, storage/network/input tests, suspend/resume evidence; retain QEMU regression coverage. |

## Boundaries and order

Stabilize the existing app corpus before expanding its claims. Keep each
kernel/API or driver increment in a focused PR with a reproducer. The
cursor controls are an initial console increment, not complete screen
buffers. ConPTY needs process attachment, pipe lifetime, resize and VT
output handling together; do not replace its failing stubs with false
success. Native Intel/AMD/NVIDIA acceleration requires a separate hardware
and driver design; the working Venus/virgl path is virtualization support.

GPU work must preserve CPU rendering when no compatible GPU exists.
Storage tests operate on disposable images. Physical hardware results
cannot be inferred from QEMU. Update source fragments and compatibility
notes only to the level actually demonstrated by the tests.
