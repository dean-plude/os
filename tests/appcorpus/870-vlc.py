# VLC media player plays the H.264 and AAC MP4 the ffmpeg test made
# (C:\Apps\in.mp4: 30 s of SMPTE colour bars with a 440 Hz tone), looping,
# with its Qt interface: its screenshot must match tests/reference/vlc.png
# and the sound NovaOS played must hold the tone (App(sound=...)).  The
# 32-bit PortableApps package (an NSIS installer 7z unpacks; only App/vlc,
# the program itself, is kept).  VLC offers the decoder its hardware
# formats first and the display rejects each (no Direct3D converter), which
# takes seconds without KVM and repeats at every loop, so the screenshot
# waits for VLC's log (-vv, through the kernel log) to show the software
# path settled.  Windowed: runs after the console programs (870), takes the
# keyboard.
import os, re, shutil, subprocess, tempfile, time

DOC = 'VLC (plays an H.264 and AAC MP4 with sound)'


def unpack(app, files, dest):
    """App/vlc out of the PortableApps installer"""
    tmp = tempfile.mkdtemp(prefix='vlc-paf')
    subprocess.run(['7z', 'x', '-y', f'-o{tmp}', files[0], 'App/vlc'], check=True, stdout=subprocess.DEVNULL)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    shutil.move(os.path.join(tmp, 'App', 'vlc'), dest)
    shutil.rmtree(tmp, ignore_errors=True)


def settled(nova, echo):
    """Wait for the software decoder's pictures to reach the display (once
    a loop, after the hardware formats were tried), then let a few draw"""
    got, end = '', time.time() + 90
    while time.time() < end:
        got += nova.sr.read_new()
        if re.search(r'adapt decoder I420 to display', got):
            time.sleep(6)
            return None
        time.sleep(1)
    return 'VLC did not get to software decoding (no I420 display filter in its log)'


APP = App('VLC', '3.0.21', 'https://download2.portableapps.com/portableapps/VLCPortable/VLCPortable_3.0.21.paf.exe',
          'VLC', [Test('play an MP4', rf'start {A}\VLC\vlc.exe -vv --no-qt-privacy-ask --no-qt-updates-notif '
                       rf'--avcodec-hw=none --loop --no-video-title-show {A}\in.mp4', timeout=45)],
          unpack=unpack, gui=True, sound=(440, 3000), interact=settled)
