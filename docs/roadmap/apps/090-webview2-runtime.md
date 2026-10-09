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
  `chrometest`); ~~drawing the page without a GPU (the Basic Render
  Driver adapter, a WARP device, DirectComposition, the GPU process's
  window inside the browser's)~~ Done (`dcomptest`); ~~Windows'
  `WOW6432Node` registry view for machine-wide installs~~ Done (`regtest`).
  Still to do: drawing with ANGLE on DXVK when a GPU is there.
