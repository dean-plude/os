# powertest: the lid and a thermal zone of tests/acpi/lid-thermal.asl, with
# pc-testdev as their embedded controller; the test closes the lid, heats
# and cools the zone when powertest asks (close_lid, set_temp)
DOC = ('`powertest` (closing the lid in `tests/acpi/lid-thermal.asl` sleeps, a USB key and the lid wake it, '
       'the thermal zone\'s readings)')
TESTS = [
    Test('powertest', 'powertest', [r'powertest: \d+ passed, 0 failed', r'\[SHELL\] Lid closed: sleeping',
                                    r'\[SLEEP\] Woke up', r'\[ACPI\] Lid open',
                                    r'TZ00: 70\.0 C, at or above the passive trip point \(60\.0 C\): passive cooling on',
                                    r'TZ00: 45\.0 C, below the passive trip point \(60\.0 C\): passive cooling off'],
         acts=[(r'powertest: close the lid', close_lid), (r'powertest: heat to 70 C', set_temp(70)),
               (r'powertest: cool to 45 C', set_temp(45))],
         boot_expect=[r'\[ACPI\] SCI on IRQ \d+ \(GSI \d+\)', r'\[ACPI\] PCI interrupt routing: \d+ entries',
                      r'\[ACPI\] Lid \\_SB_\.LID0', r'\[ACPI\] Thermal zone \\_TZ_\.TZ00: 40\.0 C',
                      r'\[ACPI\] Wake device \\_SB_\.LID0 \(lid\)',
                      r'\[ACPI\] Wake device \\_SB_\.PCI0\.XHC0 \(USB controller\)',
                      r'\[USB\] [^\n]*keyboard[^\n]*wakes the machine'],
         timeout=300),
]
