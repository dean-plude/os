# Beneath a Steel Sky (free on GOG, which ships it with ScummVM) is
# played: ScummVM installs from its own installer (Inno Setup, a 32-bit
# setup program that puts the 64-bit ScummVM in place), starts the
# freeware floppy release Revolution and ScummVM publish, skips the intro
# (Esc) to the first scene and walks Robert Foster along the gantry with a
# click, which must move him.  ScummVM draws with OpenGL 1.1 (NovaOS's
# own, no driver installed).  scummvm.ini only turns off the first-run
# question about checking for updates.  Windowed, takes the keyboard.
import os, shutil, tempfile, time, zipfile

DOC = 'Beneath a Steel Sky on ScummVM (free on GOG; installs with its installer, skips the intro and walks)'

INSTALLER = 'scummvm-2026.3.0-win32.exe'
CONFIG = '[scummvm]\nupdates_check=0\n'
P = r'C:\Programs\ScummVM'


def unpack(app, files, dest):
    """The installer, the game's files in game\\ and scummvm.ini in @dest"""
    os.makedirs(os.path.join(dest, 'game'))
    shutil.copy(files[0], os.path.join(dest, INSTALLER))
    with zipfile.ZipFile(files[1]) as z:
        for n in z.namelist():
            if n.lower().startswith('sky.'):
                with z.open(n) as src, open(os.path.join(dest, 'game', n.lower()), 'wb') as out:
                    shutil.copyfileobj(src, out)
    with open(os.path.join(dest, 'scummvm.ini'), 'w', newline='\r\n') as f:
        f.write(CONFIG)


def changed(a, b, box):
    """Pixels inside @box (a screenshot's pixels) that differ between two screenshots"""
    from PIL import Image, ImageChops
    d = ImageChops.difference(Image.open(a).convert('RGB').crop(box), Image.open(b).convert('RGB').crop(box))
    return sum(1 for v in d.convert('L').getdata() if v > 48)


def walk(nova, echo):
    """Esc skips the intro; a click on the gantry's left walks Foster there"""
    nova.keys('esc')
    time.sleep(60)
    nova.keys('esc')                    # (a second press, in case the first came before the intro)
    time.sleep(60)
    work = tempfile.mkdtemp(prefix='bass')
    before, after = os.path.join(work, 'before.png'), os.path.join(work, 'after.png')
    nova.shot(before)
    nova.click(420, 360)
    time.sleep(60)
    nova.shot(after)
    moved = changed(before, after, (640, 320, 1920, 1280))
    print(f'  gantry: {moved} pixels changed after the click', flush=True)
    if moved < 2000:
        return f'Foster did not walk after the click ({moved} pixels changed)'
    return None


APP = App('Beneath a Steel Sky', '2026.3.0', 'https://downloads.scummvm.org/frs/scummvm/2026.3.0/' + INSTALLER,
          'BASS',
          [Test('install', rf'{A}\BASS\{INSTALLER} /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /DIR={P}',
                timeout=900, settle=2),
           Test('play', rf'start {P}\scummvm.exe --config={A}\BASS\scummvm.ini --path={A}\BASS\game sky', timeout=90)],
          unpack=unpack, gui=True, interact=walk,
          extra=['https://downloads.scummvm.org/frs/extras/Beneath%20a%20Steel%20Sky/BASS-Floppy-1.3.zip'])
