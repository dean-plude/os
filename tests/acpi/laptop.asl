/* A Modern Standby laptop for the devices suite's "laptop" boot
 * (tools/selftest.py starts QEMU without \_S3, as the ThinkPad T14 Gen 4's
 * firmware has none): an embedded controller holding the lid switch, a
 * battery and the AC adapter, which their methods read as a real laptop's
 * do; an LPS0 device; and a thermal zone.
 *
 * QEMU emulates no embedded controller: _HID NOVA0EC1 makes NovaOS serve
 * this one with the model of a controller in kernel/hal/ec.c (its ports
 * below are never touched).  The model raises the event whose number AML
 * writes to its last register, EVNT, as a real controller raises its own.
 * The thermal zone's _TMP, which NovaOS polls every second, passes the lid
 * position the test sets on QEMU's pc-testdev (port 0xE8: 0 open, 1
 * closed) to the controller and raises event 0x2A, whose _Q2A notifies
 * the lid. */
DefinitionBlock ("", "SSDT", 2, "NOVA", "LAPTOP", 1)
{
    OperationRegion (TDEV, SystemIO, 0xE8, 1)
    Field (TDEV, ByteAcc, NoLock, Preserve)
    {
        LIDC, 8
    }

    Scope (\_SB)
    {
        Device (EC0)
        {
            Name (_HID, "NOVA0EC1")
            Name (_CID, EisaId ("PNP0C09"))
            Name (_UID, 1)
            Name (_GPE, 0x0F)
            Name (_CRS, ResourceTemplate ()
            {
                IO (Decode16, 0x0EC0, 0x0EC0, 0x01, 0x01)      /* data */
                IO (Decode16, 0x0EC4, 0x0EC4, 0x01, 0x01)      /* command and status */
            })
            Name (ECOK, 0)                          /* the OS handles the region (_REG) */
            Method (_REG, 2)
            {
                If (Arg0 == 3) { ECOK = Arg1 }
            }
            OperationRegion (ECR, EmbeddedControl, 0, 0x100)
            Field (ECR, ByteAcc, Lock, Preserve)
            {
                LIDS, 8,                            /* 0x00: 1 closed */
                Offset (0x02),
                BSTA, 8,                            /* 0x02: battery state (_BST) */
                Offset (0x04),
                BREM, 16,                           /* 0x04: remaining, mWh */
                BRAT, 16,                           /* 0x06: rate, mW */
                BFCC, 16,                           /* 0x08: last full charge, mWh */
                ACON, 8,                            /* 0x0A: on mains */
                Offset (0xFF),
                EVNT, 8                             /* the model's "raise this event" */
            }
            /* The controller's power-on state, written through the region */
            Method (_INI)
            {
                If (ECOK)
                {
                    BFCC = 50000
                    BREM = 30000
                    BRAT = 7500
                    BSTA = 1                        /* discharging */
                    ACON = 0
                    LIDS = 0
                }
            }
            Method (_Q2A) { Notify (\_SB.LID0, 0x80) }
        }

        Device (LID0)
        {
            Name (_HID, EisaId ("PNP0C0D"))
            Method (_LID) { Return (\_SB.EC0.LIDS == 0) }
        }

        Device (BAT0)
        {
            Name (_HID, EisaId ("PNP0C0A"))
            Name (_UID, 0)
            Method (_STA) { Return (0x1F) }
            /* (a name in a Package () is a reference, not the field's value:
             * the values go in at run time) */
            Method (_BIF)
            {
                Local0 = Package () { 0, 57000, 0, 1, 15400, 5000, 2500, 1, 1,
                                      "T14 test battery", "1", "LiON", "NovaOS" }
                Local0[2] = \_SB.EC0.BFCC
                Return (Local0)
            }
            Method (_BST)
            {
                Local0 = Package () { 0, 0, 0, 15400 }
                Local0[0] = \_SB.EC0.BSTA
                Local0[1] = \_SB.EC0.BRAT
                Local0[2] = \_SB.EC0.BREM
                Return (Local0)
            }
        }

        Device (AC0)
        {
            Name (_HID, "ACPI0003")
            Method (_PSR) { Return (\_SB.EC0.ACON) }
        }

        /* Low-power S0 idle: Intel's _DSM functions 0, 3, 4, 5 and 6 */
        Device (PEPD)
        {
            Name (_HID, "INT33A1")
            Name (_CID, EisaId ("PNP0D80"))
            Name (CNT, 0)                           /* calls to functions 3 to 6 */
            Method (_DSM, 4, Serialized)
            {
                If (Arg0 == ToUUID ("c4eb40a0-6cd2-11e2-bcfd-0800200c9a66"))
                {
                    If (Arg2 == 0) { Return (Buffer (1) { 0x79 }) }
                    CNT++
                }
                Return (Buffer (1) { 0 })
            }
        }
    }

    Scope (\_TZ)
    {
        ThermalZone (TZ01)
        {
            Name (_TZP, 10)                         /* poll every 1.0 s */
            Name (LAST, 0)
            Method (_TMP)
            {
                If (LIDC != LAST)
                {
                    LAST = LIDC
                    \_SB.EC0.LIDS = LIDC
                    \_SB.EC0.EVNT = 0x2A
                }
                Return (3082)                       /* 35 C */
            }
        }
    }
}
