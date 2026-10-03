/* A laptop's I2C-HID touchpad for the self-tests (tools/selftest.py passes
 * the compiled table to QEMU with -acpitable), described the way a
 * ThinkPad's DSDT describes it: an Intel LPSS I2C controller at PCI
 * 00:15.1 with its fast-mode timings (FMCN), and on it the touchpad at
 * address 0x2C (PNP0C50), 400 kHz, its interrupt on a GPIO pin and its
 * HID descriptor at register 0x20 (_DSM function 1).  QEMU has no I2C
 * controller there, so the test checks that NovaOS reads all of this and
 * says the controller is missing. */
DefinitionBlock ("", "SSDT", 2, "NOVA", "I2CTPAD", 1)
{
    External (\_SB.PCI0, DeviceObj)

    Scope (\_SB.PCI0)
    {
        Device (I2C1)
        {
            Name (_ADR, 0x00150001)
            Method (FMCN) { Return (Package () { 0x0050, 0x00A0, 0x001E }) }

            Device (TPD0)
            {
                Name (_HID, "NOVA0C50")
                Name (_CID, "PNP0C50")
                Name (_UID, One)
                Method (_STA) { Return (0x0F) }
                Method (_CRS, 0, Serialized)
                {
                    Return (ResourceTemplate ()
                    {
                        I2cSerialBusV2 (0x002C, ControllerInitiated, 400000, AddressingMode7Bit,
                                        "\\_SB.PCI0.I2C1", 0x00, ResourceConsumer, , Exclusive, )
                        GpioInt (Level, ActiveLow, Exclusive, PullUp, 0x0000, "\\_SB.GPI0", 0x00,
                                 ResourceConsumer, , ) { 0x0115 }
                    })
                }
                Method (_DSM, 4, Serialized)
                {
                    If (Arg0 == ToUUID ("3cdff6f7-4267-4555-ad05-b30a3d8938de"))
                    {
                        If (Arg2 == Zero) { Return (Buffer () { 0x03 }) }
                        If (Arg2 == One) { Return (0x20) }
                    }
                    Return (Buffer () { 0x00 })
                }
            }

            Device (TPD1)                       /* the other touchpad model the firmware knows: absent */
            {
                Name (_HID, "NOVA0C51")
                Name (_CID, "PNP0C50")
                Name (_UID, 2)
                Method (_STA) { Return (Zero) }
            }
        }
    }
}
