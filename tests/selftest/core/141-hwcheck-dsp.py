# hwcheck, the audio DSP part: the T14 Gen 4's digital microphones sit
# behind Intel's audio DSP, which QEMU does not model.  `hwcheck` reads a
# modelled NHLT table (a two-microphone array past an SSP endpoint, two
# formats with their DMIC configuration), a modelled Sound Open Firmware
# file (extended manifest, $CPD, $AM1 module list), and boots a modelled
# DSP: core 0 power, the ROM's load request naming the code loader's
# stream, the image read back through the BDL, FW_READY and an IPC4
# FW_CONFIG request; a bad image must leave the core off.  Then the
# capture pipeline on it: the model checks each IPC4 message (two copiers
# on the DMIC and host input gateways, BIND, PAUSED, RUNNING), fills the
# host ring with a quarter-scale tone, takes the stop, and refuses the
# host copier once.  QEMU's HD Audio controller is class 04.03, so the
# real DSP is reported absent
DOC = ('`hwcheck` audio DSP: NHLT digital microphones, the SOF firmware manifest, the DSP boot '
       '(ROM, code loader, FW_READY, IPC4) and the IPC4 capture pipeline into a host ring, on a modelled DSP')
TESTS = [
    Test('hwcheck dsp', 'hwcheck', [r'ok   DSP: NHLT: the DMIC endpoint found past an SSP one, 2 microphones',
                                    r'ok   DSP: the ROM read the whole image through the code loader',
                                    r'ok   DSP: FW_READY answered, IPC4 FW_CONFIG request: firmware 2\.12\.0\.1 running, '
                                    r'2 digital microphones recording \(48000 Hz, 2 channels\)',
                                    r'ok   DSP: capture: copier \(module 2\) on DMIC gateway 0 with the NHLT blob',
                                    r'ok   DSP: capture: copier on host input gateway 6, its stream decoupled',
                                    r'ok   DSP: capture: CREATE_PIPELINE, two INIT_INSTANCE, BIND, PAUSED, host DMA on, RUNNING',
                                    r'ok   DSP: capture: samples in the host ring, position \d+, level 25%',
                                    r'ok   DSP: capture stop: PAUSED, host DMA off, RESET, DELETE_PIPELINE',
                                    r'ok   DSP: a bad image: boot fails, core 0 off',
                                    r'ok   DSP: a refused copier: the pipeline deleted, the stream recoupled',
                                    r'audio DSP on this machine: no audio DSP \(the controller is class 04\.03\)',
                                    r'hwcheck: all passed, 0 failed'], builtin=True),
]
