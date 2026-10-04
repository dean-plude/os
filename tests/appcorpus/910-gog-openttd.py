# OpenTTD (free on GOG) installs from its own installer and reaches its main
# menu: the screenshot must match tests/reference/openttd.png.  GOG's copy
# needs a GOG account to download, so the corpus takes OpenTTD's own
# Windows installer (NSIS; its manifest asks for administrator, so NovaOS
# runs it elevated) and OpenGFX, the free base graphics GOG's copy also
# ships.  The installer runs silently into C:\Programs\OpenTTD, OpenGFX goes
# into its baseset folder, and the title game is removed so the menu sits
# on a still, empty map.  openttd.cfg turns off hardware acceleration and
# the survey question; sound and music are off (-s null -m null).
# Windowed, takes the keyboard.
import os, shutil, zipfile

DOC = 'OpenTTD (free on GOG; installs with its installer and reaches its main menu)'

INSTALLER = 'openttd-15.3-windows-win64.exe'
CONFIG = '[misc]\nvideo_hw_accel = false\n\n[network]\nparticipate_survey = no\n'
P = r'C:\Programs\OpenTTD'


def unpack(app, files, dest):
    """The installer, OpenGFX's tar (out of its zip) and openttd.cfg in @dest"""
    os.makedirs(dest)
    shutil.copy(files[0], os.path.join(dest, INSTALLER))
    with zipfile.ZipFile(files[1]) as z:
        tar = next(n for n in z.namelist() if n.endswith('.tar'))
        with z.open(tar) as src, open(os.path.join(dest, os.path.basename(tar)), 'wb') as out:
            shutil.copyfileobj(src, out)
    with open(os.path.join(dest, 'openttd.cfg'), 'w', newline='\r\n') as f:
        f.write(CONFIG)


APP = App('OpenTTD', '15.3', 'https://cdn.openttd.org/openttd-releases/15.3/' + INSTALLER, 'OpenTTD',
          [Test('install', rf'{A}\OpenTTD\{INSTALLER} /S /D={P}', timeout=600, settle=2),
           Test('OpenGFX', rf'copy {A}\OpenTTD\opengfx-8.0.tar {P}\baseset', [r'1 file\(s\) copied'],
                builtin=True),
           Test('no title game', rf'del {P}\baseset\opntitle.dat', builtin=True),
           Test('main menu', rf'start {P}\openttd.exe -c {A}\OpenTTD\openttd.cfg -v win32 -s null -m null',
                timeout=240)],
          unpack=unpack, extra=['https://cdn.openttd.org/opengfx-releases/8.0/opengfx-8.0-all.zip'], gui=True)
