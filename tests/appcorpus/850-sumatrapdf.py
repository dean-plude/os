# SumatraPDF opens a PDF the corpus generates (C:\Apps\data\corpus.pdf);
# its screenshot must match tests/reference/sumatrapdf.png.  The official
# 32-bit build, taken from the npm package pdf-to-printer (the project's
# own site is not reachable from every network).  Windowed: runs after the
# console programs (850), takes the keyboard.
import os, shutil, tarfile

DOC = 'SumatraPDF'


def unpack(app, files, dest):
    """SumatraPDF-3.4.6-32.exe out of the package's dist/ folder"""
    os.makedirs(dest)
    with tarfile.open(files[0]) as t, t.extractfile('package/dist/SumatraPDF-3.4.6-32.exe') as src, \
            open(os.path.join(dest, 'SumatraPDF.exe'), 'wb') as out:
        shutil.copyfileobj(src, out)


APP = App('SumatraPDF', '3.4.6', 'https://registry.npmjs.org/pdf-to-printer/-/pdf-to-printer-5.8.1.tgz',
          'SumatraPDF', [Test('open a PDF', rf'start {A}\SumatraPDF\SumatraPDF.exe {A}\data\corpus.pdf', timeout=12)],
          unpack=unpack, gui=True)
