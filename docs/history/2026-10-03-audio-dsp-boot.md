## The audio DSP boots Sound Open Firmware (the T14's microphones, part 1)

The ThinkPad T14 Gen 4's built-in microphones are digital microphones on
Intel's audio DSP, not on the Realtek codec, so Phase 21.4 left them
silent.  This is the first of three steps to record from them
([hardware.md](../hardware.md#the-digital-microphones-behind-the-audio-dsp)).

- **NHLT.**  The ACPI NHLT table describes the microphones: the endpoint
  of link type PDM that records, its microphone array type (two or four
  microphones), its formats and, per format, the configuration blob the
  firmware needs to clock them.  `kernel/drivers/sof.c` reads it and logs
  what it found (`[DSP] NHLT: ...`).
- **The firmware.**  `tools/fetch_sof_firmware.py` downloads Intel's
  signed Sound Open Firmware from the SOF project's sof-bin release
  (v2026.09.1, BSD-3-Clause with Intel's firmware licence, SHA-256
  checked) into `third_party/sof-bin`, which git ignores; the build puts
  it at `C:\Windows\Firmware\Intel\sof-ipc4\rpl\sof-rpl.ri`.  The driver
  checks the file: its extended manifest, the `$CPD` partition the ROM
  takes, and the `$AM1` module list (the copier, which the next step
  uses, is module 4 in this release).
- **The boot.**  On a Tiger Lake to Raptor Lake controller with its DSP
  on, a background thread powers the DSP's first core while it is held
  in reset, asks the ROM to load the firmware from a host DMA stream,
  lets the core run, streams the image to the ROM over the last output
  stream, decoupled from the link through the processing pipe (with the
  software position limit at the image's end, link power saving off
  while loading, and an input stream held for the DSP's full current),
  waits for the firmware to start, answers its FW_READY message and asks
  it for its FW_CONFIG over IPC4.  Any failure switches the core off and
  restores the processing pipe, so playback and the headset microphone
  are never affected; each step is logged for the boot log on the stick.
- **Tests.**  QEMU has no audio DSP, so `hwcheck` (core self-test
  `hwcheck dsp`) runs the NHLT reader on a modelled two-microphone table,
  the manifest reader on a modelled firmware file and the whole boot on a
  modelled DSP that reads the image back through the code loader's BDL
  and compares it, answers the IPC4 request, and must be left off after a
  bad image.  Its last line says what the real DSP did; on the T14 it
  should name the running firmware and two microphones (unverified: not
  yet run on the machine).
- Not yet: the IPC4 capture pipeline (DMIC gateway copier to a host input
  stream) and the recording device for it.
