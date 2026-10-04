- The WebView2 runtime (Roblox's login page and many other programs show
  web content with it): ~~Edge Update's Windows APIs~~ Done
  (`edgeupdtest`); ~~MSXML 6~~ Done (`msxmltest`); ~~rpcrt4's NDR engine
  for COM proxy/stub DLLs~~ Done (`ndrtest`); ~~COM calls between
  processes~~ Done (`comoop`); ~~Edge Update's check of Microsoft's
  signature on the runtime's package~~ Done (`authtest`); ~~Edge Update
  seeing its own install running~~ Done (`proclisttest`); ~~delete on
  close~~ Done (`filetest`); ~~the runtime's setup unpacking its archive,
  `wer.dll`~~ Done (`wvsetuptest`).  Still to do: the folder the setup is
  refused inside its install folder ("Access is denied"), Windows'
  `WOW6432Node` registry view, then the Chromium runtime itself.
