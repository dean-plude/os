#!/usr/bin/env python3
"""Cross-build Mesa's Venus Vulkan driver (vulkan_virtio.dll) for NovaOS.

    tools/build_venus.py OUT_DIR [--work DIR] [--tarball FILE] [--arch x64|x86]

Venus is Mesa's Vulkan driver for virtio-gpu: it encodes a program's
Vulkan calls and the host's renderer (virglrenderer under QEMU) runs them
on the host's GPU.  NovaOS's own back end (third_party/mesa-venus) talks to
the kernel's virtio-gpu driver through NtNovaGpuCtl.  This script fetches
the Mesa release named below (or takes --tarball), checks its SHA-256,
applies third_party/mesa-venus/novaos.patch, adds vn_renderer_nova.c and
builds the driver with MinGW-w64 through Meson, 64- and 32-bit.

OUT_DIR receives venus.7z, which is what the App Store's "Venus" entry
installs: x64\\vulkan_virtio.dll with x64\\virtio_icd.x86_64.json, and
x86\\vulkan_virtio.dll with x86\\virtio_icd.x86.json (kernel/apps/store.c).
The CI publishes it beside nova.iso on the "latest" release.

Needs meson (1.4 or newer), ninja, Python's mako and yaml modules,
glslangValidator, bison, flex, 7z, and MinGW-w64 gcc and g++ for x86-64
and i686.  Later runs reuse the work directory (default OUT_DIR/work):
only what changed recompiles.
"""
import argparse, hashlib, os, shutil, subprocess, sys, tarfile, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TP = os.path.join(ROOT, 'third_party', 'mesa-venus')

VERSION = '26.2.4'
SHA256 = 'bce5f7fbebb934373b86c999a064d52fb5065878dc57f287f95346648ec832e9'
URLS = [f'https://archive.mesa3d.org/mesa-{VERSION}.tar.xz',
        f'http://archive.ubuntu.com/ubuntu/pool/main/m/mesa/mesa_{VERSION}.orig.tar.xz']

# Only Venus: no OpenGL, no other drivers, no LLVM, no optional libraries
OPTIONS = ['--wrap-mode=nofallback', '-Dbuildtype=release', '-Db_ndebug=true',
           '-Dplatforms=windows', '-Dvulkan-drivers=virtio', '-Dgallium-drivers=',
           '-Dopengl=false', '-Dgles1=disabled', '-Dgles2=disabled', '-Degl=disabled', '-Dglx=disabled',
           '-Dllvm=disabled', '-Dvideo-codecs=', '-Dbuild-tests=false', '-Dzlib=disabled',
           '-Dzstd=disabled', '-Dxmlconfig=disabled', '-Dexpat=disabled', '-Dshader-cache=disabled',
           # (vk_icdNegotiateLoaderICDInterfaceVersion declines when there is no 3D GPU)
           '-Dc_args=-DVK_ICD_PRESENT_HOOK=vn_renderer_nova_present']

ARCHES = {   # NovaOS name: (MinGW triplet, Meson cpu family, manifest name, library_arch)
    'x64': ('x86_64-w64-mingw32', 'x86_64', 'virtio_icd.x86_64.json', '64'),
    'x86': ('i686-w64-mingw32', 'x86', 'virtio_icd.x86.json', '32'),
}


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def fetch(work):
    path = os.path.join(work, f'mesa-{VERSION}.tar.xz')
    if os.path.exists(path) and sha256(path) == SHA256:
        return path
    for u in URLS:
        try:
            print(f'downloading {u}', flush=True)
            urllib.request.urlretrieve(u, path + '.part')
        except OSError as e:
            print(f'  {e}', flush=True)
            continue
        if sha256(path + '.part') == SHA256:
            os.replace(path + '.part', path)
            return path
        print('  SHA-256 mismatch', flush=True)
    sys.exit(f'could not get Mesa {VERSION}')


def source(work, tarball):
    """The patched Mesa tree, extracted again when the patch or back end changed"""
    stamp = hashlib.sha256()
    for f in ('novaos.patch', 'vn_renderer_nova.c'):
        stamp.update(open(os.path.join(TP, f), 'rb').read())
    stamp = stamp.hexdigest()
    src = os.path.join(work, f'mesa-{VERSION}')
    mark = os.path.join(src, '.novaos')
    if os.path.exists(mark) and open(mark).read() == stamp:
        return src
    shutil.rmtree(src, ignore_errors=True)
    if tarball and sha256(tarball) != SHA256:
        sys.exit(f'{tarball} is not Mesa {VERSION}')
    with tarfile.open(tarball or fetch(work)) as t:
        t.extractall(work, filter='tar')
    subprocess.run(['patch', '-p1', '-s', '-i', os.path.join(TP, 'novaos.patch')], cwd=src, check=True)
    shutil.copy(os.path.join(TP, 'vn_renderer_nova.c'), os.path.join(src, 'src', 'virtio', 'vulkan'))
    open(mark, 'w').write(stamp)
    return src


def build(src, work, arch):
    triplet, family, _, _ = ARCHES[arch]
    cross = os.path.join(work, f'cross-{arch}.txt')
    with open(cross, 'w') as f:
        f.write(f"""[binaries]
c = '{triplet}-gcc'
cpp = '{triplet}-g++'
ar = '{triplet}-ar'
strip = '{triplet}-strip'
windres = '{triplet}-windres'
pkg-config = 'false'
[host_machine]
system = 'windows'
cpu_family = '{family}'
cpu = '{family}'
endian = 'little'
""")
        if arch == 'x86':    # export vk_icd* undecorated (no @N), as the loader looks them up
            f.write("[built-in options]\nc_link_args = ['-Wl,--kill-at']\ncpp_link_args = ['-Wl,--kill-at']\n")
    bdir = os.path.join(work, f'build-{arch}')
    if not os.path.exists(os.path.join(bdir, 'build.ninja')):
        shutil.rmtree(bdir, ignore_errors=True)
        subprocess.run(['meson', 'setup', bdir, src, f'--cross-file={cross}'] + OPTIONS, check=True)
    subprocess.run(['ninja', '-C', bdir, 'src/virtio/vulkan/libvulkan_virtio.dll'], check=True)
    return os.path.join(bdir, 'src', 'virtio', 'vulkan', 'libvulkan_virtio.dll')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('out')
    ap.add_argument('--work')
    ap.add_argument('--tarball', help=f'Mesa {VERSION}\'s source tarball (instead of downloading it)')
    ap.add_argument('--arch', action='append', choices=tuple(ARCHES), help='only this one (default: both)')
    a = ap.parse_args()
    work = os.path.abspath(a.work or os.path.join(a.out, 'work'))
    os.makedirs(work, exist_ok=True)
    src = source(work, a.tarball)
    stage = os.path.join(work, 'stage')
    shutil.rmtree(stage, ignore_errors=True)
    for arch in a.arch or ARCHES:
        triplet, _, manifest, bits = ARCHES[arch]
        dll = build(src, work, arch)
        os.makedirs(os.path.join(stage, arch))
        subprocess.run([f'{triplet}-strip', '-o', os.path.join(stage, arch, 'vulkan_virtio.dll'), dll], check=True)
        with open(os.path.join(stage, arch, manifest), 'w') as f:
            f.write('{\n    "file_format_version": "1.0.1",\n    "ICD": {\n'
                    '        "library_path": "vulkan_virtio.dll",\n'
                    f'        "api_version": "1.4.354",\n        "library_arch": "{bits}"\n    }}\n}}\n')
    archive = os.path.join(os.path.abspath(a.out), 'venus.7z')
    if os.path.exists(archive):
        os.unlink(archive)
    subprocess.run(['7z', 'a', '-bd', '-mx=9', archive, '.'], cwd=stage, check=True, stdout=subprocess.DEVNULL)
    print(f'{archive}: {os.path.getsize(archive) // 1024} KB', flush=True)


if __name__ == '__main__':
    main()
