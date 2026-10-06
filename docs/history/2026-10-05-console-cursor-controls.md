## Classic console cursor controls reach the Terminal

`SetConsoleCursorPosition` used to return success without moving anything;
`SetConsoleCursorInfo` ignored its argument, and `GetConsoleCursorInfo`
always reported a visible 25 percent cursor. The calls now go through
`NtNovaConsole`, with the same fixed-width arguments on x64 and x86.

- Positions are checked against the Terminal's grid, including negative
  coordinates. Only console output handles are accepted.
- Positioning and visibility controls are queued through the same writer
  lock as ordinary text. A complete control sequence waits for room before
  entering the ring; cancellation cannot leave half an escape sequence.
- Cursor size (1 through 100 percent) and visibility are stored per
  console, shared by duplicated handles and processes attached to it.
  The Terminal uses the size when painting its cursor, and recognizes
  visibility controls even before it enters screen mode.
- `consolecursortest` joins both native core suites. The host test
  `python3 -m unittest discover -s tests/tools -p test_console_cursor.py`
  exercises the actual queue code: emitted sequences, boundaries, shared
  state, cancellation and a full ring that must drain before a control.

This is an incremental console implementation. `GetConsoleScreenBufferInfo`
still does not report the current cursor position; cell readback, fills,
scrolling, alternate screen buffers and ConPTY remain open. Raw VT cursor
visibility changes are rendered but do not update the classic API's stored
visibility. No new application is marked compatible from this change alone.
