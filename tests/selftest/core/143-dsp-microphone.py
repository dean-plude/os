# The T14's microphone array as a recording device, on QEMU, which has no
# audio DSP: `hwcheck mic` boots a live modelled DSP the way the real one
# boots (NHLT, firmware, IPC4 capture pipeline) and attaches its ring as
# "Microphone Array (DSP model)"; the model's DMA writes a 1 kHz tone
# while the pipeline runs.  waveIn, WASAPI and DirectSound must list it,
# `soundtest record` must hear the tone through waveIn, `hwcheck mic
# sleep` takes the model's power as S3 does and the resume path must boot
# it again, and WASAPI capture must still hear the tone after it; then
# `hwcheck`'s own modelled DSP must not disturb the live one.  The
# tone is the model's, not the host's microphone, so these run without
# PulseAudio too
DSP_HZ = 1000


def dsp_tone(guest, ms):
    check = recording(guest, DSP_HZ, ms)
    check.needs_mic = False
    return check


DOC = ('`hwcheck mic`: a modelled audio DSP as the "Microphone Array" recording device (`waveIn`, WASAPI and '
       'DirectSound list it, `soundtest record` and `capture` hear its tone, before and after a modelled sleep)')
TESTS = [
    Test('hwcheck mic', 'hwcheck mic', [r'ok   DSP model: Microphone Array \(DSP model\) attached \(48000 Hz, 2 channels\): '
                                        r'firmware 2\.12\.0\.1 running, 2 digital microphones',
                                        r'hwcheck mic: all passed, 0 failed'], builtin=True,
         boot_expect=[r'\[AUDIO\] Recording from Microphone Array \(DSP model\)']),
    Test('dsp microphone devices', 'soundtest info', [r'"Microphone Array \(DSP model\)" channels=2']),
    Test('dsp microphone endpoints', 'soundtest endpoints', [r'Microphone Array \(DSP model\)']),
    Test('dsp microphone dsenum', 'soundtest dsenum', [r'Microphone Array \(DSP model\)']),
    Test('dsp microphone record', r'soundtest record C:\dsp.wav 2000 dev=Array', [r'recorded 88200 samples'],
         check=dsp_tone(r'C:\dsp.wav', 1500)),
    Test('hwcheck mic sleep', 'hwcheck mic sleep',
         [r'ok   DSP model: after sleep the firmware booted again \(boot 2\) and the pipeline came back: '
          r'.*paused while no program records', r'hwcheck mic sleep: all passed, 0 failed'], builtin=True),
    Test('dsp microphone capture', r'soundtest capture C:\dspcap.wav 1000 dev=Array',
         [r'captured \d+ frames .* at -12\.0 dB'], check=dsp_tone(r'C:\dspcap.wav', 800), settle=3),
    Test('hwcheck with the dsp model', 'hwcheck',
         [r'ok   DSP: after sleep: the firmware booted again \(boot 2\)',
          r'audio DSP on this machine: firmware 2\.12\.0\.1 running, 2 digital microphones \(48000 Hz, 2 channels\) '
          r'\(the modelled DSP of hwcheck mic\), paused while no program records',
          r'hwcheck: all passed, 0 failed'], builtin=True),
]
