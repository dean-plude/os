# KeePassXC (Qt 5) opens a password database the corpus writes next to it
# (passwords.kdbx, master password "novaos"); the password is typed into
# its unlock screen and the screenshot of the unlocked database (groups,
# entries, the selected entry's details) must match
# tests/reference/keepassxc.png.  The portable zip from KeePassXC's GitHub
# releases; a config/keepassxc.ini beside it turns off the first-run
# question about checking for updates.  Windowed, takes the keyboard.
import base64, os, shutil, time, zipfile

DOC = 'KeePassXC'

# A KDBX 4 database (Argon2d, made with pykeepass): groups Root and NovaOS,
# entries Router (in Root), GitHub and Email (in NovaOS)
DATABASE = (
    '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'
)
CONFIG = '[General]\nConfigVersion=2\nUpdateCheckMessageShown=true\n\n[GUI]\nCheckForUpdates=false\n'


def unpack(app, files, dest):
    """The zip's one folder, the settings and the database"""
    with zipfile.ZipFile(files[0]) as z:
        for m in z.infolist():
            parts = m.filename.split('/')[1:]
            if not parts or not parts[-1]:
                continue
            path = os.path.join(dest, *parts)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with z.open(m) as src, open(path, 'wb') as out:
                shutil.copyfileobj(src, out)
    os.makedirs(os.path.join(dest, 'config'), exist_ok=True)
    with open(os.path.join(dest, 'config', 'keepassxc.ini'), 'w', newline='\r\n') as f:
        f.write(CONFIG)
    with open(os.path.join(dest, 'passwords.kdbx'), 'wb') as f:
        f.write(base64.b64decode(''.join(DATABASE)))


def unlock(nova, echo):
    """Type the master password; deriving the key (Argon2d, 64 MiB) takes a while"""
    nova.qmp.type('novaos\n')
    time.sleep(45)
    return None


APP = App('KeePassXC', '2.7.12',
          'https://github.com/keepassxreboot/keepassxc/releases/download/2.7.12/KeePassXC-2.7.12-Win64.zip',
          'KeePassXC', [Test('unlock a database', rf'start {A}\KeePassXC\KeePassXC.exe {A}\KeePassXC\passwords.kdbx',
                             timeout=60)],
          unpack=unpack, gui=True, interact=unlock)
