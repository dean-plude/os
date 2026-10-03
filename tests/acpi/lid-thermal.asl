/* A laptop's lid and thermal zone for the self-tests (tools/selftest.py
 * passes the compiled table to QEMU with -acpitable).  The "embedded
 * controller" is QEMU's pc-testdev: the test sets the lid (port 0xE8:
 * 0 open, 1 closed) and the temperature (port 0xE9, in degrees C; 0 reads
 * as 40 C) from the QEMU monitor.  As a real embedded controller would
 * raise an event, the thermal zone's _TMP, which the OS polls every
 * second, notifies the lid when it has changed.  The xHCI controller (at
 * 00:05.0 in the test boot) is described as a wake device. */
DefinitionBlock ("", "SSDT", 2, "NOVA", "LIDTHERM", 1)
{
    OperationRegion (TDEV, SystemIO, 0xE8, 2)
    Field (TDEV, ByteAcc, NoLock, Preserve)
    {
        LIDC, 8,
        TMPC, 8
    }

    Scope (\_SB)
    {
        Device (LID0)
        {
            Name (_HID, EisaId ("PNP0C0D"))
            Name (LAST, 0)                          /* the lid as last notified */
            Name (_PRW, Package () { 0x0E, 3 })     /* opening it wakes the machine */
            Method (_LID) { Return (LIDC == 0) }
        }

    }

    External (\_SB.PCI0, DeviceObj)
    Scope (\_SB.PCI0)
    {
        Device (XHC0)
        {
            Name (_ADR, 0x00050000)
            Name (_PRW, Package () { 0x0D, 3 })     /* PME# from the USB controller */
        }
    }

    Scope (\_TZ)
    {
        ThermalZone (TZ00)
        {
            Name (_TZP, 10)                          /* poll every 1.0 s */
            Method (_TMP)
            {
                If (LIDC != \_SB.LID0.LAST)
                {
                    \_SB.LID0.LAST = LIDC
                    Notify (\_SB.LID0, 0x80)
                }
                Local0 = TMPC
                If (Local0 == 0) { Local0 = 40 }
                Return (2732 + Local0 * 10)
            }
            Method (_PSV) { Return (3332) }          /* 60 C */
            Method (_HOT) { Return (3632) }          /* 90 C */
            Method (_CRT) { Return (3682) }          /* 95 C */
        }
    }
}
