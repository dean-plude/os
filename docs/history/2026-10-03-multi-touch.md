## Multi-touch

Touch screens with more than one finger now work, and programs get them
the way Windows hands them out: `WM_TOUCH` or `WM_POINTER*`.

- **USB digitizers** (`usbhid.c`): a report descriptor with Digitizer
  page finger collections (Tip Switch, Contact Identifier, X and Y), and
  usually a Contact Count, is a multi-touch screen ("multi-touch screen"
  in the log).  Reports are read in hybrid mode too, where a frame's
  contacts come spread over several reports and the first carries the
  count.  Contacts that stop being reported are lifted, and unplugging the
  screen lifts them all.  `usbcheck` runs canned descriptors through it.
- **virtio multi-touch** (`virtio_input.c`, new): the virtio input device
  (`virtio-multitouch-pci` in QEMU) speaks the Linux evdev multi-touch
  protocol B (slots, tracking ids, `ABS_MT_POSITION_X`/`Y`); its axis
  ranges come from the device's config space and it is set up again after
  S3.
- **The desktop** (`desktop.c`): a frame of contacts goes to the program
  window under the first contact; anywhere else (the shell, the taskbar,
  built-in apps) the primary contact acts as an absolute mouse.
- **Programs** (`user32` `pointer.c`): a window that called
  `RegisterTouchWindow` gets `WM_TOUCH` with `GetTouchInputInfo`
  (hundredths of a pixel, `TOUCHEVENTF_DOWN`/`MOVE`/`UP`/`PRIMARY`);
  others get `WM_POINTERDOWN`/`UPDATE`/`UP` with `GetPointerInfo`,
  `GetPointerType` (`PT_TOUCH`), `GetPointerTouchInfo` and the frame
  functions, and `DefWindowProc` promotes the primary contact to
  `WM_LBUTTONDOWN`/`MOUSEMOVE`/`UP`, as on Windows.
  `GetSystemMetrics(SM_DIGITIZER)` and `SM_MAXIMUMTOUCHES` report the
  screen.
- **Tests**: a new `devices` self-test suite boots with a virtio
  multi-touch screen; `touchtest` puts two fingers down on a
  `RegisterTouchWindow` window, moves and lifts them, then taps a window
  that uses pointers, and checks what both get.
- Not yet: gestures (`WM_GESTURE`), pens, the Input Mode feature report
  some USB screens need before they leave mouse mode, and
  `GetMessageExtraInfo`'s touch signature on promoted mouse messages.
