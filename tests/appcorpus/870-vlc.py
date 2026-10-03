# VLC media player plays the H.264 and AAC MP4 the ffmpeg test made
# (C:\Apps\in.mp4: 30 s of SMPTE colour bars with a 440 Hz tone), looping,
# with its Qt interface: its screenshot must match tests/reference/vlc.png
# and the sound NovaOS played must hold the tone (App(sound=...)).  The
# 32-bit PortableApps package (an NSIS installer 7z unpacks; only App/vlc,
# the program itself, is kept), from PortableApps' SourceForge archive: its
# own download host keeps only the current release.  VLC offers the decoder its hardware
# formats first and the display rejects each (no Direct3D converter), which
# takes seconds without KVM, so the screenshot waits for the colour bars to
# show in the window.  Run with -vv: VLC's verbose log (some 1,200 lines
# while the clip starts, each written twice, to stderr and to
# OutputDebugString, both through the kernel log) must not make its audio
# run late, so the sound check still wants an unbroken tone.  Windowed:
# runs after the console programs (870), takes the keyboard.
import os, shutil, subprocess, tempfile, time

DOC = 'VLC (plays an H.264 and AAC MP4 with sound)'


def unpack(app, files, dest):
    """App/vlc out of the PortableApps installer"""
    tmp = tempfile.mkdtemp(prefix='vlc-paf')
    subprocess.run(['7z', 'x', '-y', f'-o{tmp}', files[0], 'App/vlc'], check=True, stdout=subprocess.DEVNULL)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    shutil.move(os.path.join(tmp, 'App', 'vlc'), dest)
    shutil.rmtree(tmp, ignore_errors=True)


def settled(nova, echo):
    """Wait for the colour bars to reach VLC's window (the video area shows
    the cone until the software decoder's pictures arrive), let the sound
    run, and wait for the bars again: the clip may have looped meanwhile,
    and VLC shows the cone again while it starts over"""
    shot = os.path.join(tempfile.mkdtemp(prefix='vlc-shot'), 'shot.png')

    def bars(secs):
        from PIL import Image
        end = time.time() + secs
        while time.time() < end:
            nova.shot(shot)
            im = Image.open(shot).convert('RGB')
            k = im.width // 1280                        # the screen's scale
            r, g, b = im.getpixel((480 * k, 330 * k))   # inside the bars, left of the cone
            if max(r, g, b) > 60:
                return True
            time.sleep(2)
        return False

    if not bars(90):
        return 'VLC did not show the video (the window stayed dark)'
    time.sleep(6)
    if not bars(60):
        return 'VLC stopped showing the video'
    return None


APP = App('VLC', '3.0.21', 'https://downloads.sourceforge.net/project/portableapps/VLC%20Media%20Player%20Portable/VLCPortable_3.0.21.paf.exe',
          'VLC', [Test('play an MP4', rf'start {A}\VLC\vlc.exe -vv --no-qt-privacy-ask --no-qt-updates-notif '
                       rf'--avcodec-hw=none --loop --no-video-title-show {A}\in.mp4', timeout=45)],
          unpack=unpack, gui=True, sound=(440, 3000), interact=settled)
