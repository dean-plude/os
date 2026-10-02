- The Win32 console screen-buffer calls (`SetConsoleCursorPosition`,
  `FillConsoleOutputCharacter`... are still no-ops), so programs that draw
  through them rather than VT sequences work and `less` can be the real
  one (git pages through NovaOS's own `less` today).
