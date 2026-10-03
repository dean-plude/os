# Inkscape 0.91 (GTK 2) starts with a new document: its menus, tool
# bars, toolbox, rulers, canvas with the empty page, palette and status
# bar must match tests/reference/inkscape.png.  The build is conda-forge's
# win-64 package (a MinGW build with its own GTK 2, cairo and pango, about
# 70 DLLs); inkscape.org's own downloads are not reachable from CI.
# Windowed; it takes a while to load its GTK modules and icons.
import os, shutil, tarfile

DOC = 'Inkscape'


def unpack(app, files, dest):
    """The package's Library/inkscape folder"""
    prefix = 'Library/inkscape/'
    with tarfile.open(files[0], 'r:bz2') as t:
        for m in t:
            if not m.isfile() or not m.name.startswith(prefix):
                continue
            path = os.path.join(dest, *m.name[len(prefix):].split('/'))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with t.extractfile(m) as src, open(path, 'wb') as out:
                shutil.copyfileobj(src, out)


APP = App('Inkscape', '0.91',
          'https://conda.anaconda.org/conda-forge/win-64/inkscape-0.91-0.tar.bz2',
          'Inkscape', [Test('new document', rf'start {A}\Inkscape\inkscape.exe', timeout=180)],
          unpack=unpack, gui=True)
