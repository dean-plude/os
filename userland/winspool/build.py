# winspool.drv's build hook (tools/build_userland.py calls it with itself
# as b): the print spooler's DLL is installed under its Windows name,
# winspool.drv, which programs import it by.
import os


def link(b, odir, objs, deps, base):
    b.link_dll(odir, 'winspool', objs, deps, base)
    sysdir = 'System32' if b.ARCH == 'x64' else 'SysWOW64'
    b.built.append((f'\\Windows\\{sysdir}\\winspool.drv', os.path.join(odir, 'winspool.dll')))
