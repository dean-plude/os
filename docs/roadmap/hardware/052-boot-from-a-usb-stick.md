- ~~Start from a USB stick~~ Done (Phase 21.2): `nova.iso` is a USB stick
  image as well as a disc (its EFI System Partition is a GPT partition
  too), runs live from the stick and offers Install NovaOS, with the
  firmware's GOP framebuffer as the display, and writes its log into
  `\EFI\NOVA\bootlog.txt` on the stick (no serial port needed).  Checked
  in QEMU (`usbboot` and `cdboot` in the devices suite); on the
  reference ThinkPad T14 Gen 4 the check is by hand: start from the
  stick with Secure Boot off, reach the desktop, read the log on another
  computer.
