## Cave Story with a game pad

The ninth game is Cave Story again, now played with a game pad, as the
first game that reads one through DirectInput rather than XInput or
winmm.  The game opens the first joystick DirectInput 7 lists
(`EnumDevices(DIDEVTYPE_JOYSTICK)`), reads it with `c_dfDIJoystick` and
`GetDeviceState`, and maps its buttons by number: on NovaOS, as on
Windows, an Xbox controller's A is button 1 and B button 2, so B jumps
and says OK and the left stick walks.  NovaOS's `dinput.dll` (built from
`dinput8`'s source with the DirectInput 3 to 7 interfaces) needed no
change: the pad starts a new game, takes the opening talk to the Start
Point cave and walks Quote across it.  The nightly corpus plays it so
(`tests/appcorpus/946-cave-story-pad.py`) with an Xbox 360 pad from
`tools/padpeer.py`, and the cave must match its reference picture.
