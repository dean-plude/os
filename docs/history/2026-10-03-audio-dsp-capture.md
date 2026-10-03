## The audio DSP records the T14's microphones (part 2)

The second of three steps to the ThinkPad T14 Gen 4's built-in
microphones ([part 1](#the-audio-dsp-boots-sound-open-firmware-the-t14s-microphones-part-1),
[hardware.md](../hardware.md#the-digital-microphones-behind-the-audio-dsp)):
once Sound Open Firmware runs on the audio DSP, NovaOS has it record
the digital microphones into memory it owns.

- **The pipeline.**  `kernel/drivers/sof.c` sends the firmware the IPC4
  messages for one pipeline: CREATE_PIPELINE; INIT_INSTANCE of a copier
  on the DMIC gateway, whose configuration is NHLT's blob for the first
  format; INIT_INSTANCE of a second copier on the host input gateway of
  the last HD Audio input stream, which turns the samples into 16-bit
  ones; BIND of the first to the second; then SET_PIPELINE_STATE to
  PAUSED and, once the stream's DMA runs, RUNNING.  The payload layouts
  follow SOF's BSD-licensed headers (`ipc4_copier_module_cfg`, the base
  module configuration, audio format and gateway node ID); the copier's
  module ID is read from the firmware's module list.
- **The ring.**  The input stream is decoupled from the link (the
  processing pipe), so the DSP's host DMA fills its 128 KiB ring and
  its position register says how far; `SofCaptureRing()` and
  `SofCapturePosition()` hand both to the mixer for the next step.  If
  the firmware refuses a message, the pipeline is deleted and the stream
  given back to the link, and the firmware keeps running.
- **Tests.**  The modelled DSP behind `hwcheck` (core self-test
  `hwcheck dsp`) now checks every pipeline message against the layouts
  (node IDs, formats, buffer sizes, the NHLT blob, the order of states),
  fills the ring with a quarter-scale square wave while the pipeline
  runs, and refuses the host copier once to check the clean-up.  On the
  T14, `hwcheck`'s last line should say the microphones are recording
  and give their level (unverified: not yet run on the machine).
- Not yet: the recording device over the ring ("Microphone Array" in the
  Sound settings, `waveIn`, Audacity) and the DSP booted again after
  sleep.
