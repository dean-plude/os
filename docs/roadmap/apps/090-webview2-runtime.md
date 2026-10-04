- The WebView2 runtime (Roblox's login page and many other programs show
  web content with it): ~~Edge Update's Windows APIs~~ Done
  (`edgeupdtest`); ~~MSXML 6~~ Done (`msxmltest`); ~~rpcrt4's NDR engine
  for COM proxy/stub DLLs~~ Done (`ndrtest`); ~~COM calls between
  processes~~ Done (`comoop`).  Still to do: Edge Update's check of
  Microsoft's signature on the runtime's package, Windows'
  `WOW6432Node` registry view, then the runtime's setup and the Chromium
  runtime itself.
