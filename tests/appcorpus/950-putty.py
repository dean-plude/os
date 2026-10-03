# PuTTY makes a raw connection to the echo server tools/appcorpus.py runs
# on the host (10.0.2.2 on QEMU's user network), types a line the server
# must receive, and its screenshot must match tests/reference/putty.png.
# Built from the source release with MinGW (cmake, gcc-mingw-w64-x86-64):
# PuTTY's download site is not reachable from every network.  The build is
# kept in the cache.  Last (950): it takes the keyboard.
import os, shutil, subprocess, tarfile, time

DOC = 'PuTTY'


def build(archive):
    """putty.exe from the source tarball, built next to it in the cache"""
    cache = os.path.dirname(archive)
    exe = os.path.join(cache, 'putty-build', 'putty.exe')
    if os.path.exists(exe):
        return exe
    src = os.path.join(cache, 'putty-src')
    shutil.rmtree(src, ignore_errors=True)
    with tarfile.open(archive) as t:
        t.extractall(src)
    src = os.path.join(src, os.listdir(src)[0])
    bdir = os.path.join(cache, 'putty-build')
    shutil.rmtree(bdir, ignore_errors=True)
    subprocess.run(['cmake', '-S', src, '-B', bdir, '-DCMAKE_SYSTEM_NAME=Windows', '-DCMAKE_BUILD_TYPE=Release',
                    '-DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc', '-DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres'],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run(['make', '-C', bdir, '-j', str(os.cpu_count() or 2), 'putty'], check=True, stdout=subprocess.DEVNULL)
    return exe


def unpack(app, files, dest):
    os.makedirs(dest)
    shutil.copy(build(files[0]), os.path.join(dest, 'putty.exe'))


def type_line(nova, echo):
    """A line typed into the terminal must reach the echo server"""
    line = 'hello from NovaOS'
    nova.qmp.type(line + '\n')
    time.sleep(4)
    if line not in echo.lines:
        return f'the echo server did not receive the typed line (got {echo.lines!r})'
    return None


APP = App('PuTTY', '0.81', 'http://archive.ubuntu.com/ubuntu/pool/universe/p/putty/putty_0.81.orig.tar.gz',
          'PuTTY', [Test('raw connection', rf'start {A}\PuTTY\putty.exe -raw 10.0.2.2 -P {ECHO_PORT}', timeout=15)],
          unpack=unpack, gui=True, net=True, interact=type_line)
