- **Media players and editors**: VLC plays video with sound and Audacity
  records and saves, on the same WASAPI, GDI and message-loop code paths
  real Windows programs take (Qt's `WH_GETMESSAGE` hook, wxWidgets'
  buffered painting).
