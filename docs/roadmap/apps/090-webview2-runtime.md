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
  process~~ Done (`wvstarttest`, `unwindtest`).  Still to do: the
  controller and a page (the GPU process's Direct3D 11 adapter, the
  browser's restart, `CreateCoreWebView2Controller`), and Windows'
  `WOW6432Node` registry view for machine-wide installs.
