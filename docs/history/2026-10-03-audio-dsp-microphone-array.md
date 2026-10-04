## The T14's microphones are a recording device (part 3)

The last of three steps to the ThinkPad T14 Gen 4's built-in
microphones ([part 2](#the-audio-dsp-records-the-t14s-microphones-part-2),
[hardware.md](../hardware.md#the-digital-microphones-behind-the-audio-dsp)):
what the audio DSP records reaches programs.

- **Microphone Array (DSP).**  Once the capture pipeline runs,
  `kernel/drivers/sof.c` attaches its ring to the mixer as a recording
  device, the default input from then on as on Windows.  Settings >
  Sound, `waveIn`, WASAPI and DirectSound list it, so Audacity can
  record from it.  Inputs of the mixer may now have any channel count:
  a microphone array's four become stereo (the even channels left, the
  odd ones right), and its 16 kHz or 48 kHz is converted as before.
- **Paused while nothing records.**  The DSP's thread now stays: when
  the last recorder stops it pauses the pipeline (SET_PIPELINE_STATE
  PAUSED, the stream's DMA off) and runs it again when one starts.
- **After sleep.**  S3 takes the DSP's power, its firmware and its
  pipeline; `SofResume()`, after `HdaResume()` on wake, has the thread
  boot the DSP again from the same firmware file and build the pipeline
  again, and the mixer reads the new ring from where the DSP starts
  writing.
- **Tests.**  `hwcheck` checks the pause, the run again and the boot
  again after a modelled sleep on its modelled DSP.  `hwcheck mic` boots
  a live model whose DMA writes a 1 kHz tone and attaches it as
  "Microphone Array (DSP model)": the new core self-test `hwcheck mic`
  has `waveIn`, WASAPI and DirectSound list it and `soundtest record`
  and `capture` hear the tone, before and after `hwcheck mic sleep`
  takes its power.  On the T14 the check is by hand (hardware.md;
  unverified: not yet run on the machine).
