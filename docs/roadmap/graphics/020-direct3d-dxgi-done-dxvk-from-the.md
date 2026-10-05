- ~~**Direct3D**, DXGI~~ Done: DXVK from the App Store is the system
  Direct3D 8–11 on Mesa's lavapipe Vulkan, through NovaOS's own
  `vulkan-1.dll` and behind NovaOS's own `d3d9.dll`, `d3d11.dll` and
  `dxgi.dll` (which load without DXVK and report no Direct3D); see
  [Direct3D](HISTORY.md#direct3d-dxvk-on-mesas-vulkan).  Without DXVK,
  `dxgi.dll` lists Windows' Microsoft Basic Render Driver and `d3d11.dll`
  gives a WARP device that maps and copies textures, with `dcomp.dll`
  showing its swap chains, for Chromium's software compositor
  ([WebView2 draws its page](HISTORY.md#webview2-draws-its-page)).
