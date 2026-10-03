# hwcheck: the drivers for the reference laptop's devices that QEMU can't
# show (Phase 21.4), run against modelled devices: the HD Audio controller
# matching on the ThinkPad T14 Gen 4's IDs (its controller is class 04.01
# with the audio DSP on), the codec setup and headphone jack handling on a
# modelled Realtek ALC257, and the I2C-HID protocol on a modelled
# touchpad.  The boot's ACPI table tests/acpi/i2c-touchpad.asl describes a
# touchpad on an I2C controller QEMU doesn't have: NovaOS must read the
# address, speed, controller and HID descriptor register from the
# namespace (skipping the model whose _STA says absent) and report the
# controller missing
DOC = ('`hwcheck` (HD Audio controller matching, a modelled ALC257 codec with its headphone jack, a modelled '
       'I2C-HID touchpad); the touchpad in the boot\'s ACPI table is found and its missing controller reported')
TESTS = [
    Test('hwcheck', 'hwcheck', [r'ok   ALC257: headphones plugged in: speaker pin off',
                                r'ok   touchpad: finger report ignored, mouse reports move and click',
                                r'hwcheck: all passed, 0 failed'], builtin=True,
         boot_expect=[r'\[ACPI\] I2C-HID device \\_SB_\.PCI0\.I2C1\.TPD0 \(NOVA0C50\): address 0x2c at 400 kHz '
                      r'on \\_SB\.PCI0\.I2C1 \(PCI 00:15\.1\), HID descriptor at 0x0020, GPIO interrupt',
                      r'\[I2C\] \\_SB_\.PCI0\.I2C1\.TPD0 \(NOVA0C50\): no I2C controller at PCI 00:15\.1',
                      r'\[HDA\] Codec 0 \([0-9a-f]{4}:[0-9a-f]{4}, subsystem [0-9a-f]{4}:[0-9a-f]{4}\): \d+ output']),
]
