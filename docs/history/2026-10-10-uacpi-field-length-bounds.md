## uACPI field lengths are bit counts, not byte spans

The uACPI hardening in #270 made every PkgLength fit inside its enclosing
code.  A ReservedField or NamedField length inside a `Field` is also
encoded as a PkgLength, but it counts bits, so a 16-bit or longer field
runs past the end of the Field's own bytes and the table was refused with
"invalid PkgLength bounds".  The embedded controller, lid, thermal zone,
battery and I2C touchpad tables of the test machine failed to load, and the
core and devices self-tests (powertest, hwcheck, touchpad interrupt, ec
battery, lid sleep) failed on main.  Only the tracked PkgLength of an
operation that really is a byte span (Scope, Device, Method, If, Else,
Buffer, Package and the like) is checked against its enclosing code now;
field lengths keep the overflow check only.
Looking for the byte after a finished If or Else is bounded by the enclosing
code block only again: with the new bound the If's own package ended exactly
there, so an Else after it was never seen and the PCI interrupt routing table
(`_PRT`, which chooses with If/Else) could not be built.
