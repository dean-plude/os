# battery: the boot that runs the core suite has an unplugged AC adapter and
# a battery (tests/acpi/battery.asl)
DOC = '`battery` (against the battery in `tests/acpi/battery.asl`)'
TESTS = [
    Test('battery', 'battery', [r'Power source: battery', r'Battery: 75%', r'Time left: 3 h 00 min',
                                r'SystemBatteryState: present 1, AC 0, charging 0, discharging 1']),
]
