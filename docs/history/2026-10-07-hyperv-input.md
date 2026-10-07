## Keyboard and mouse under Hyper-V

A Generation 2 Hyper-V VM has no PS/2 controller and no USB: its keyboard
and mouse are VMBus channels, so NovaOS started there with neither.  The
kernel now finds Hyper-V (CPUID "Microsoft Hv"), sets up the hypercall
page and the SynIC on CPU 0, connects to VMBus (version 5.3 down to 3.0),
lists the devices the host offers and opens the synthetic keyboard and
mouse.  The keyboard's set-1 scan codes go where the PS/2 keyboard's do;
the mouse's HID report descriptor and reports go through the HID decoding
USB and I2C mice use, as an absolute pointer.  The device poll thread reads
both channels' rings.  A host test runs the ring buffer and both protocols
without a VM.  Storage, network and the synthetic video device have no
driver yet ([hyperv.md](../hyperv.md)).
