- Win32 console screen buffers: cursor positioning, size and visibility
  controls now reach the Terminal (`consolecursortest`, x64 and x86).
  Still to do: current cursor position in `GetConsoleScreenBufferInfo`,
  cell reads and writes, fills, scrolling, text attributes and independent
  screen buffers. Verify a real console-screen API application before
  claiming full coverage; Git still uses NovaOS's own `less`.
