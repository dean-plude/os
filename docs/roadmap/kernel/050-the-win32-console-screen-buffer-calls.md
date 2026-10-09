-   Win32 console screen-buffer APIs maintain per-console cell contents,
  cursor position and text attributes. Reads, writes, fills, scrolls, size
  changes and independent buffers are exercised by `consolecursortest` on
  x64 and x86. The Terminal redraws the active buffer; Git still uses
  NovaOS's own `less`.
