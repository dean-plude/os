- The WebView2 runtime (Roblox's login page and many other programs show
  web content with it): ~~Edge Update's Windows APIs~~ Done
  (`edgeupdtest`); ~~MSXML 6~~ Done (`msxmltest`); ~~rpcrt4's NDR engine
  for COM proxy/stub DLLs~~ Done (`ndrtest`); ~~COM calls between
  processes~~ Done (`comoop`); ~~Edge Update's check of Microsoft's
  signature on the runtime's package~~ Done (`authtest`); ~~Edge Update
  seeing its own install running~~ Done (`proclisttest`); ~~delete on
  close~~ Done (`filetest`); ~~the runtime's setup unpacking its archive,
  `wer.dll`~~ Done (`wvsetuptest`); ~~the setup's permissions on its
  install folder~~ Done (`acltest`); ~~starting the runtime's browser
  process~~ Done (`wvstarttest`, `unwindtest`); ~~the browser keeping
  its host's connection (pipe process ids)~~ Done (`pipetest`); ~~windows
  of one process inside another's (the host's `SetParent` and
  `SetWindowPos` on the browser's window, another process drawing into
  it; the controller, a page and a script)~~ Done (`xpwin`,
  `chrometest`).  Still to do: drawing the page (a DXGI factory with
  Windows' software adapter when there is no GPU, for Chromium's
  software output; for ANGLE, the GPU process's child window inside the
  browser's and a DXGI swap chain presenting to it), and Windows'
  `WOW6432Node` registry view for machine-wide installs.
