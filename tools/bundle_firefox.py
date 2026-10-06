"""Stage Mozilla's unmodified Windows Firefox payload for the OS image."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import urllib.request

VERSION = '157.0'
RELEASE = f'https://archive.mozilla.org/pub/firefox/releases/{VERSION}/'
INSTALLER = f'win64/en-US/Firefox Setup {VERSION}.exe'
DEST = r'\Programs\Mozilla Firefox\core'


def installer_digest(checksums):
    matches = []
    for line in checksums.splitlines():
        digest, sep, path = line.partition('  ')
        if sep and path.strip() == INSTALLER:
            if len(digest) != 128 or any(c not in '0123456789abcdefABCDEF' for c in digest):
                raise ValueError('invalid Mozilla SHA-512 digest')
            matches.append(digest.lower())
    if len(matches) != 1:
        raise ValueError('installer must appear exactly once in Mozilla SHA512SUMS')
    return matches[0]


def sha512(path):
    h = hashlib.sha512()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def download(url, target, digest=None):
    target = Path(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    if digest and target.is_file() and sha512(target) == digest:
        return
    fd, temporary = tempfile.mkstemp(prefix=target.name + '.', dir=target.parent)
    os.close(fd)
    try:
        with urllib.request.urlopen(url, timeout=120) as src, open(temporary, 'wb') as dst:
            shutil.copyfileobj(src, dst)
        if not Path(temporary).stat().st_size or (digest and sha512(temporary) != digest):
            raise ValueError('Firefox download is empty or its SHA-512 does not match Mozilla')
        os.replace(temporary, target)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def payload_files(core):
    core = Path(core)
    if not (core / 'firefox.exe').is_file() or not (core / 'xul.dll').is_file():
        raise ValueError('Firefox payload lacks firefox.exe or xul.dll')
    result = []
    for path in sorted(core.rglob('*')):
        if path.is_symlink():
            raise ValueError('symlink in Firefox payload')
        if path.is_file():
            relative = path.relative_to(core).as_posix()
            result.append((DEST + '\\' + relative.replace('/', '\\'), str(path)))
    return result


def stage(out):
    cache = Path(os.environ.get('NOVA_FIREFOX_CACHE', Path.home() / '.cache/novaos/firefox')) / VERSION
    cache.mkdir(parents=True, exist_ok=True)
    sums = cache / 'SHA512SUMS'
    # Retrieve the release's checksum manifest over Mozilla HTTPS. It is
    # pinned to VERSION; never use the mutable "latest" installer endpoint.
    download(RELEASE + 'SHA512SUMS', sums)
    digest = installer_digest(sums.read_text())
    installer = cache / 'Firefox-Setup.exe'
    url = RELEASE + INSTALLER.replace(' ', '%20')
    download(url, installer, digest)
    extractor = shutil.which('7z') or shutil.which('7zz')
    if not extractor:
        raise RuntimeError('bundled Firefox requires 7z (install p7zip-full or 7zip)')
    destination = Path(out) / 'firefox'
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='firefox-', dir=destination.parent) as work:
        subprocess.run([extractor, 'x', '-y', f'-o{work}', str(installer)],
                       check=True, stdout=subprocess.DEVNULL)
        core = Path(work) / 'core'
        payload_files(core)
        provenance = {'version': VERSION, 'source_url': url, 'sha512': digest,
                      'checksums_url': RELEASE + 'SHA512SUMS', 'destination': DEST}
        (Path(work) / 'provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
        if destination.exists():
            shutil.rmtree(destination)
        shutil.move(work, destination)
    print(f'Bundled Firefox {VERSION}: Mozilla SHA-512 verified')
    return payload_files(destination / 'core')
