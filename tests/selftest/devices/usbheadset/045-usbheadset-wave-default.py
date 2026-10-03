# usbheadset, waveOut and waveIn device 0: after 040's restart the surround
# headset is the default output and "Microphone (Test Headset)" the default
# input, so they must be waveOut and waveIn device 0, as on Windows (the
# others follow, oldest first), and DRVM_MAPPER_PREFERRED_GET must name
# device 0 for both.  When Settings' choice moves to the full-speed
# speaker, it becomes device 0 and the surround headset moves down.
# (szPname holds 31 characters: "Speakers (Test Surround Headset".)

TESTS = [
    Test('wave device 0', 'soundtest info',
         [r'waveOut devices: 3', r'  0: r=0 "Speakers \(Test Surround Headset', r'  0: r=0 "Microphone \(Test Headset\)"',
          r'preferred waveOut device: 0 \(r=0\), waveIn device: 0 \(r=0\)']),
    Test('wave default moves', 'soundtest default out "Test Speaker"',
         [r'default output: "Speakers \(Test Speaker\)" chosen']),
    Test('wave device 0 moved', 'soundtest info',
         [r'  0: r=0 "Speakers \(Test Speaker\)"', r'  [12]: r=0 "Speakers \(Test Surround Headset']),
]
