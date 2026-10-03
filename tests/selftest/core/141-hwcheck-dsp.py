# hwcheck, the audio DSP part: the T14 Gen 4's digital microphones sit
# behind Intel's audio DSP, which QEMU does not model.  `hwcheck` reads a
# modelled NHLT table (a two-microphone array past an SSP endpoint, two
# formats with their DMIC configuration), a modelled Sound Open Firmware
# file (extended manifest, $CPD, $AM1 module list), and boots a modelled
# DSP: core 0 power, the ROM's load request naming the code loader's
# stream, the image read back through the BDL, FW_READY and an IPC4
# FW_CONFIG request; a bad image must leave the core off.  QEMU's HD Audio
# controller is class 04.03, so the real DSP is reported absent
DOC = ('`hwcheck` audio DSP: NHLT digital microphones, the SOF firmware manifest, and the DSP boot '
       '(ROM, code loader, FW_READY, IPC4) on a modelled DSP')
TESTS = [
    Test('hwcheck dsp', 'hwcheck', [r'ok   DSP: NHLT: the DMIC endpoint found past an SSP one, 2 microphones',
                                    r'ok   DSP: the ROM read the whole image through the code loader',
                                    r'ok   DSP: FW_READY answered, IPC4 FW_CONFIG request: firmware 2\.12\.0\.1 running',
                                    r'ok   DSP: a bad image: boot fails, core 0 off',
                                    r'audio DSP on this machine: no audio DSP \(the controller is class 04\.03\)',
                                    r'hwcheck: all passed, 0 failed'], builtin=True),
]
