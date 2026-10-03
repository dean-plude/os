## Krita leftovers: asyncio, keyboard shortcuts, a painted stroke

What #122 left open on Krita 5.3.4, fixed in NovaOS:

- **Overlapped sockets that wait.**  Krita's Python scripter plugin
  imports `asyncio`, whose Windows event loop (the proactor) asks Winsock
  for `AcceptEx`, `ConnectEx` and the other extension functions through
  `WSAIoctl(SIO_GET_EXTENSION_FUNCTION_POINTER)`; NovaOS answered
  `WSAEOPNOTSUPP` and the import failed.  `ws2_32` now hands out
  `AcceptEx`, `ConnectEx`, `DisconnectEx`, `GetAcceptExSockaddrs`,
  `TransmitFile`, `WSARecvMsg` and `WSASendMsg`, and an overlapped
  request that has to wait (an `AcceptEx` with no connection yet, a
  `WSARecv` with nothing to read) returns `WSA_IO_PENDING` and finishes
  later on the socket's I/O completion port, as on Windows.  The request
  keeps its own copy of the caller's `WSABUF` array (it often lives on the
  caller's stack), `AcceptEx` completes on the listening socket's port,
  `CancelIoEx` and `closesocket` end it with `ERROR_OPERATION_ABORTED`,
  and the port is captured when the request goes pending, so a completion
  still arrives after the socket's handle value has been reused.  The
  kernel's socket control gained "accept into this socket" for `AcceptEx`.
  New self-test `overlaptest` (64- and 32-bit); `asyncio.run` with a TCP
  echo server and client works under NovaOS's Python.
- **Ctrl+N and the other shortcuts.**  `GetKeyState` gave 0x8000 for a
  key that is down; Windows gives 0xFF80 (0xFF81 when toggled), and Qt
  reads the modifiers with `GetKeyState(VK_LCONTROL) & 0x80`, so in every
  Qt program Ctrl, Shift and Alt never looked held and Ctrl+N was a plain
  N.  `user32` also never sent `WM_ACTIVATEAPP`; every top-level window of
  a program now gets it (TRUE) before `WM_NCACTIVATE` when the program
  gains the foreground, and FALSE when it loses it.  `inputtest` checks
  both.
- **A stroke in the corpus test.**  The Krita corpus test now opens its
  new image with Ctrl+N (no click first), drags a stroke across the canvas
  with the default brush, checks the canvas pixels changed, and takes the
  stroke away with Ctrl+Z.  The test
  harness (`tools/novarun.py`) gained `drag`.

Still open: Krita needs OpenGL 2 or later (its Qt Quick widgets build
shaders), so it still needs the App Store's Mesa 3D.
