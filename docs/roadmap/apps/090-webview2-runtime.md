- The WebView2 runtime (Roblox's login page and many other programs show
  web content with it): ~~Edge Update's Windows APIs~~ Done
  (`edgeupdtest`); ~~MSXML 6~~ Done (`msxmltest`); ~~rpcrt4's NDR engine
  for COM proxy/stub DLLs~~ Done (`ndrtest`).  Still to do: COM calls
  between processes (ole32 marshaling over a channel, `LocalServer32`
  servers started on demand, oleaut32's `BSTR`/`VARIANT` marshaling and
  the `IDispatch` proxy), Windows' `WOW6432Node` registry view, then the
  runtime's setup and the Chromium runtime itself.
