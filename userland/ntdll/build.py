# ntdll.dll's build hook (tools/build_userland.py calls it with itself as b):
# every Nt function is exported a second time under its Zw name, at the
# same address, as Windows' ntdll does.  Programs that make system calls
# themselves (anti-cheat and sandbox code) look the stubs up by their Zw
# names; without them they read no service number and call service 0.
import re, subprocess


def link(b, odir, objs, deps, base):
    seen, extra = set(), []
    for o in objs:
        r = subprocess.run([b.llvm_tool('llvm-readobj'), '--coff-directives', o], capture_output=True, text=True)
        for m in re.finditer(r'/EXPORT:"?([^"\s,=]+)"?(?=[\s,]|$)', r.stdout, re.I):
            sym, name = m.group(1), b.undecorate(m.group(1))
            if name.startswith('Nt') and name[2:3].isupper() and name not in seen:
                seen.add(name)
                extra.append(f'/export:Zw{name[2:]}={sym}')
    b.link_dll(odir, 'ntdll', objs, deps, base, extra)
