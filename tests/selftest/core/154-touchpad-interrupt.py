# touchpad interrupt: the boot's ACPI table tests/acpi/i2c-touchpad.asl
# describes the chipset's GPIO controller as a Raptor Lake laptop's DSDT
# does (_HID NOVA1055: the model of an INTC1055 in kernel/hal/gpio.c, four
# communities, IRQ 14 through the I/O APIC) and the touchpad's GpioInt on
# its pin 277.  NovaOS must find the controller, route its interrupt and
# read the touchpad's pin; `hwcheck` then connects the modelled touchpad
# to that pin: the pad is set up as a level-triggered, inverted GPIO
# input, nothing interrupts while the touchpad is quiet, and a tap and a
# two-finger scroll are read when the interrupt comes (the controller's
# vector, as the I/O APIC would deliver it), the line let go and the pin
# unmasked again.  Pins outside every pad group and pads in a native
# function are refused.  QEMU has no Intel GPIO controller to do this with
DOC = ('`hwcheck` touchpad interrupt: a modelled Intel GPIO controller from the ACPI tables, its interrupt routed, '
       'the touchpad\'s GpioInt pin set up and its reports read on the interrupt instead of polling')
TESTS = [
    Test('touchpad interrupt', 'hwcheck',
         [r'ok   touchpad interrupt: the ACPI touchpad\'s GpioInt: pin 277 of \\_SB_\.GPI0, level, active low',
          r'ok   touchpad interrupt: a pin in no pad group is refused',
          r'ok   touchpad interrupt: a pad in its native function is refused',
          r'ok   touchpad interrupt: pin 277 connected: GPP_C21 on \\_SB_\.GPI0',
          r'ok   touchpad interrupt: pad set up as a GPIO input, level, inverted',
          r'ok   touchpad interrupt: no report, no interrupt',
          r'ok   touchpad interrupt: one-finger tap read on its interrupt .*: m1,0,0 m0,0,0',
          r'ok   touchpad interrupt: two fingers 6 mm down read on its interrupt .*: m0,0,0/z1 m0,0,0/z1',
          r'hwcheck: all passed, 0 failed'], builtin=True,
         boot_expect=[r'\[GPIO\] \\_SB_\.GPI0 \(NOVA1055\): the self-tests\' GPIO controller \(a model, no registers\), '
                      r'Tiger Lake-LP layout, 4 communities, 11 pad groups, IRQ 14 \(GSI 14\)',
                      r'\[ACPI\] I2C-HID device \\_SB_\.PCI0\.I2C1\.TPD0 \(NOVA0C50\): .*GPIO interrupt: pin 277 of '
                      r'\\_SB_\.GPI0, level, active low']),
]
