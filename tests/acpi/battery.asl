/* A laptop's power devices for the self-tests (tools/selftest.py passes the
 * compiled table to QEMU with -acpitable): an AC adapter that is unplugged
 * and one battery, in mWh, discharging at 75% with 3 hours left */
DefinitionBlock ("", "SSDT", 2, "NOVA", "BATTERY", 1)
{
    Scope (\_SB)
    {
        Device (ADP0)
        {
            Name (_HID, "ACPI0003")
            Name (_PCL, Package () { \_SB })
            Method (_STA) { Return (0x0F) }
            Method (_PSR) { Return (0) }
        }
        Device (BAT0)
        {
            Name (_HID, EisaId ("PNP0C0A"))
            Name (_UID, 1)
            Name (_PCL, Package () { \_SB })
            Method (_STA) { Return (0x1F) }
            Method (_BIF)
            {
                Return (Package () {
                    0,          /* power unit: mWh */
                    50000,      /* design capacity */
                    48000,      /* last full charge */
                    1,          /* rechargeable */
                    11100,      /* design voltage, mV */
                    4800, 2400, /* warning, low */
                    100, 100,   /* granularity */
                    "NOVA0", "0001", "LION", "NovaOS"
                })
            }
            Method (_BST)
            {
                Return (Package () {
                    1,          /* discharging */
                    12000,      /* rate, mW */
                    36000,      /* remaining, mWh: 75% of 48000, 3 h at 12 W */
                    11100       /* voltage, mV */
                })
            }
        }
    }
}
