#!/usr/bin/env python3
"""Build NovaOS's Windows userland and embed it into the kernel.

    tools/build_userland.py [--jobs N] OUT_DIR GENERATED_C KERNEL_SYSCALL_H
    tools/build_userland.py --check      (load the DLL and program manifests only)

Independent DLLs, programs and objects build concurrently: at most N
compiler/linker processes run at once (--jobs N or -j N; default the CPU
count; --jobs 1 builds one step at a time).  A DLL links after the DLLs it
depends on, and the output files are the same whatever N is.  The 64-bit
pass, the 32-bit pass and NetSurf's objects share that budget and build side
by side (NetSurf links once the 64-bit import libraries it needs exist); a
failed command's output is printed in one piece, headed by its pass
([x64], [x86] or [netsurf]).

Compiles, with clang --target=x86_64-pc-windows-msvc and lld-link:
  ntdll.dll, kernel32.dll, msvcrt.dll, ...  -> C:\\Windows\\System32
  crt/crt0.c (static startup code) + programs/*.c -> C:\\Programs\\*.exe
    (with programs/NAME.rc compiled by llvm-rc: the program's icon)
  samples/* -> C:\\Pictures
  the NetSurf browser (tools/build_netsurf.py) -> C:\\Programs\\NetSurf
and writes GENERATED_C: every file pulled in with .incbin plus a table the
kernel uses to install them on drive C: at boot.  Set NOVA_NO_NETSURF=1 to
leave the browser out (it is most of the build time and image size).
"""
import os, re, subprocess, sys, threading, types

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_netsurf

HERE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'userland')
def parse_args(argv):
    """(positional arguments, --check, jobs)"""
    pos, jobs, it = [], os.cpu_count() or 4, iter(argv)
    for a in it:
        if a in ('--jobs', '-j'):
            a = '--jobs=' + next(it, '')
        elif a.startswith('-j') and a[2:].isdigit():
            a = '--jobs=' + a[2:]
        if a.startswith('--jobs='):
            if not a[7:].isdigit() or int(a[7:]) < 1:
                raise SystemExit('--jobs takes a number of at least 1')
            jobs = int(a[7:])
        elif a != '--check':
            pos.append(a)
    return pos, '--check' in argv, jobs

ARGS, CHECK, JOBS = parse_args(sys.argv[1:])
if CHECK:
    out = gen_c = syscall_h = inc_gen = None
else:
    if len(ARGS) != 3:
        raise SystemExit(__doc__)
    out, gen_c, syscall_h = ARGS
    inc_gen = os.path.join(out, 'include')
    os.makedirs(inc_gen, exist_ok=True)

TARGETS = {'x64': 'x86_64-pc-windows-msvc', 'x86': 'i686-pc-windows-msvc'}
COMMON_FLAGS = ['-O2', '-ffreestanding', '-nostdlibinc',
                '-fno-stack-protector', '-mno-stack-arg-probe', '-fms-extensions', '-fasync-exceptions',
                '-Wall', '-Wno-unused-function', '-Werror=implicit-function-declaration',
                '-isystem', os.path.join(HERE, 'include', 'posix'), '-I', os.path.join(HERE, 'include'), '-I', str(inc_gen)]
# The pass a build step belongs to: x64 (System32) or x86 (SysWOW64, for
# 32-bit programs).  The passes run side by side, so the architecture is the
# running task's (this module's ARCH attribute, which build hooks read as
# b.ARCH, is a property of the calling thread; arch() is the same in here).
_task = threading.local()
def arch():
    return getattr(_task, 'arch', 'x64')
class _Module(types.ModuleType):
    ARCH = property(lambda self: arch())
sys.modules[__name__].__class__ = _Module
def cflags():
    extra = ['-msse2'] if arch() == 'x86' else []
    return ['--target=' + TARGETS[arch()]] + extra + COMMON_FLAGS

# Each system DLL is registered by its own userland/NAME/dll.json (see
# docs/building.md, "Adding a DLL or program"), so a new DLL is a new
# directory, not an edit to a list here:
#   deps      the DLLs it links against (built first)
#   base      x64 load address; base_x86 the 32-bit one.  Leave both out
#             and the DLL gets a free 16 MiB slot (AUTO_X64 / AUTO_X86),
#             so two changes adding DLLs never pick the same address
#   sources   directories its .c files come from (default: its own)
#   entry     "DllMain" for DLLs with an entry point
#             (and userland/NAME/NAME.rc, if there is one, is linked in)
#   tlssup    true: link lib/tlssup.c (implicit TLS)
#   x64_only  true: not built for SysWOW64
#   ordinals  {name: ordinal}: Windows' export ordinals, for DLLs
#             programs import from by number
# and, when it needs more than that, userland/NAME/build.py (hooks the
# build calls with this module: cflags(b), objs(b, odir) and link(b, odir,
# objs, deps, base); see userland/secur32/build.py and msvcrt/build.py).
# Programs (userland/programs/NAME.c or .cpp) may have a NAME.json:
#   x86       true: also built for 32 bits (C:\Programs\x86)
#   system    true: installed in C:\Windows\System32 (SysWOW64)
#   libs      DLLs it links against beyond PROGRAM_LIBS
#   selftest  true: a self-test program README lists
#   msstl     true: a C++ program on the C++ standard library (msvcp140.dll,
#             Microsoft's STL headers; see userland/msvcp140/build.py)
import json
SLOT = 0x01000000
AUTO_X64 = (0x7FFD00000000, 0x7FFE00000000)
AUTO_X86 = (0x97000000, 0xC0000000)

def load_manifests():
    """{name: manifest} for every userland/*/dll.json, in link order (each
    DLL after the DLLs it depends on), bases filled in"""
    found = {}
    for d in sorted(os.listdir(HERE)):
        f = os.path.join(HERE, d, 'dll.json')
        if os.path.isfile(f):
            try:
                m = json.load(open(f))
            except ValueError as e:
                raise SystemExit(f'{f}: {e}')
            for k in ('base', 'base_x86'):
                if k in m:
                    m[k] = int(m[k], 16)
            hook = os.path.join(HERE, d, 'build.py')
            m['hook'] = hook if os.path.isfile(hook) else None
            found[d] = m
    for arch, key, (lo, hi) in (('x64', 'base', AUTO_X64), ('x86', 'base_x86', AUTO_X86)):
        taken = {}
        for n, m in found.items():
            if key in m:
                if m[key] in taken:
                    raise SystemExit(f'userland/{n}/dll.json: {key} {m[key]:#x} is also '
                                     f'userland/{taken[m[key]]}\'s; leave it out to get a free one')
                taken[m[key]] = n
        slot = lo
        for n in sorted(found):
            if key in found[n]:
                continue
            while any(b <= slot < b + SLOT or slot <= b < slot + SLOT for b in taken):
                slot += SLOT
            if slot >= hi:
                raise SystemExit(f'no free {arch} DLL slot left for {n}')
            found[n][key] = slot
            taken[slot] = n
    order, state = [], {}
    def visit(n, chain):
        if state.get(n) == 'done':
            return
        if n not in found:
            raise SystemExit(f'userland/{chain[-1]}/dll.json: no DLL "{n}" (no userland/{n}/dll.json)')
        if state.get(n) == 'busy':
            raise SystemExit('DLL dependency cycle: ' + ' -> '.join(chain + [n]))
        state[n] = 'busy'
        for dep in found[n].get('deps', []):
            visit(dep, chain + [n])
        state[n] = 'done'
        order.append(n)
    for n in sorted(found):
        visit(n, [n])
    return {n: found[n] for n in order}

def load_programs():
    """{name: NAME.json's settings} for userland/programs"""
    progs = {}
    for f in sorted(os.listdir(os.path.join(HERE, 'programs'))):
        if f.endswith('.json'):
            try:
                progs[f[:-5]] = json.load(open(os.path.join(HERE, 'programs', f)))
            except ValueError as e:
                raise SystemExit(f'userland/programs/{f}: {e}')
    return progs

def load_hook(name, path):
    import importlib.util
    spec = importlib.util.spec_from_file_location(f'nova_dll_{name.replace("-", "_")}', path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod

DLLS = load_manifests()
PROGRAMS = load_programs()
HOOKS = {n: load_hook(n, m['hook']) for n, m in DLLS.items() if m['hook']}
# the DLLs every program links against
PROGRAM_LIBS = ['msvcrt', 'kernel32', 'ntdll', 'ws2_32', 'user32', 'gdi32', 'testdll', 'vcruntime140',
                'advapi32', 'ole32', 'oleaut32', 'comctl32', 'shell32', 'msi', 'winmm']
if CHECK:                                 # CI: the manifests load (bases, dependencies), nothing is built
    print(f'userland: {len(DLLS)} DLLs, {len(PROGRAMS)} program manifests: OK')
    sys.exit(0)

SLOTS = threading.BoundedSemaphore(JOBS)      # compiler/linker processes running at once
LOG = threading.Lock()                        # a failed command's output is written in one piece

def fail(text):
    with LOG:
        sys.stderr.write(f'[{arch()}] {text}')
        sys.stderr.flush()
    sys.exit(1)

def run(cmd):
    with SLOTS:
        r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        fail(' '.join(cmd) + '\n' + r.stdout + r.stderr)

def cc(src, obj, extra=()):
    run(['clang'] + cflags() + list(extra) + ['-c', src, '-o', obj])

# 1. syscall numbers from the kernel
nums = {}
for m in re.finditer(r'#define\s+SYSCALL_(Nt\w+)\s+(0x[0-9A-Fa-f]+)', open(syscall_h).read()):
    nums[m.group(1)] = m.group(2)
with open(os.path.join(inc_gen, 'syscall_numbers.h'), 'w') as f:
    f.write('/* generated from kernel/ke/syscall.h */\n#pragma once\n')
    for k, v in sorted(nums.items()):
        f.write(f'#define SYS_{k} {v}\n')

MUSL = os.path.join(os.path.dirname(HERE), 'third_party', 'musl')

def llvm_tool(t):
    import shutil
    for v in [''] + [f'-{n}' for n in range(30, 13, -1)]:
        if shutil.which(t + v):
            return t + v
    raise SystemExit(f'{t} not found (install llvm)')

def musl_math_objs(odir):
    """musl's libm (third_party/musl/src/math and the C99 complex functions
    in src/complex), compiled once for the C runtime DLLs; returns
    (objects, exported names)"""
    objs, names = [], set()
    flags = ['--target=' + TARGETS[arch()], '-O2', '-ffreestanding', '-nostdlibinc', '-fno-builtin',
             '-D_GNU_SOURCE', '-w', '-I', os.path.join(MUSL, 'include'), '-I', os.path.join(MUSL, 'src', 'internal'),
             '-I', os.path.join(HERE, 'include')]
    if arch() == 'x86':
        flags.append('-msse2')
    mdir = os.path.join(odir, 'musl')
    os.makedirs(mdir, exist_ok=True)
    for sub in ('math', 'complex'):
        srcdir = os.path.join(MUSL, 'src', sub)
        for src in sorted(os.listdir(srcdir)):
            if src.endswith('.c'):
                obj = os.path.join(mdir, src[:-2] + '.obj')
                run(['clang'] + flags + ['-c', os.path.join(srcdir, src), '-o', obj])
                objs.append(obj)
    # every public function (musl's internal helpers start with "__")
    r = subprocess.run([llvm_tool('llvm-nm'), '--defined-only', '--extern-only'] + objs,
                       capture_output=True, text=True)
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in 'TW':
            n = undecorate(parts[2])
            if not n.startswith('__'):
                names.add(n)
    return objs, sorted(names)

def undecorate(sym):
    """a C symbol's name as exported (x86: _name, _name@N)"""
    if arch() == 'x86':
        m = re.match(r'^_([A-Za-z_]\w*?)(@\d+)?$', sym)
        return m.group(1) if m else sym
    return sym

def hook(name, fn):
    """userland/NAME/build.py's function @fn, or None"""
    return getattr(HOOKS.get(name), fn, None)

def dll_objs(odir, name, srcdirs):
    objs = []
    extra = hook(name, 'cflags')(sys.modules[__name__]) if hook(name, 'cflags') else []
    for d in srcdirs:
        srcdir = os.path.join(HERE, d)
        for src in sorted(os.listdir(srcdir)):
            if src.endswith('.c'):
                obj = os.path.join(odir, f'{name}_{d}_{src[:-2]}.obj')
                cc(os.path.join(srcdir, src), obj, extra)
                objs.append(obj)
    return objs

def compile_many(srcs, odir, prefix, flags, headers=(), compiler='clang'):
    """compile @srcs in parallel into ODIR/PREFIXNAME.obj (for a DLL's
    build.py: third-party libraries); an object newer than its source and
    every one of @headers is reused.  Returns the objects, in order"""
    from concurrent.futures import ThreadPoolExecutor
    newest_h = max([os.stat(h).st_mtime for h in headers] or [0])
    objs = [os.path.join(odir, prefix + os.path.splitext(os.path.basename(s))[0] + '.obj') for s in srcs]
    jobs = [(s, o) for s, o in zip(srcs, objs)
            if not (os.path.exists(o) and os.stat(o).st_mtime > max(os.stat(s).st_mtime, newest_h))]
    def one(job):
        with SLOTS:
            r = subprocess.run([compiler] + list(flags) + ['-c', job[0], '-o', job[1]], capture_output=True, text=True)
        return None if r.returncode == 0 else job[0] + ':\n' + r.stderr
    with ThreadPoolExecutor(max_workers=JOBS) as ex:
        errors = [e for e in ex.map(one, jobs) if e]
    if errors:
        fail('\n'.join(errors[:4]))
    return objs

def defined_names(objs):
    r = subprocess.run([llvm_tool('llvm-nm'), '--defined-only', '--extern-only'] + objs, capture_output=True, text=True)
    return {undecorate(l.split()[-1]) for l in r.stdout.splitlines() if l.strip()}

def ordinal_exports(name, objs):
    """/export:NAME,@N for each ordinal-table name the DLL defines"""
    table = DLLS.get(name, {}).get('ordinals')
    if not table or arch() == 'x86':                # x86: in the .def (x86_def)
        return []
    defined = defined_names(objs)
    return [f'/export:{n},@{o}' for n, o in sorted(table.items(), key=lambda x: x[1]) if n in defined]

def x86_def(odir, name, objs):
    """32-bit DLLs export stdcall functions undecorated, as Windows' do:
    a .def naming each _Name@N the objects export as plain Name (and the
    ordinal-table names with their ordinals)"""
    names = []
    for o in objs:
        r = subprocess.run([llvm_tool('llvm-readobj'), '--coff-directives', o], capture_output=True, text=True)
        for m in re.finditer(r'/EXPORT:"?([^"\s,=]+)"?(?=[\s,]|$)', r.stdout, re.I):
            d = re.match(r'^_([A-Za-z_]\w*)@\d+$', m.group(1))
            if d and d.group(1) not in names:
                names.append(d.group(1))
    table = DLLS.get(name, {}).get('ordinals') or {}
    defined = defined_names(objs) if table else set()
    lines = [f'  {n} @{table[n]}' if n in table and n in defined else f'  {n}' for n in names]
    lines += [f'  {n} @{o}' for n, o in sorted(table.items(), key=lambda x: x[1]) if n in defined and n not in names]
    path = os.path.join(odir, name + '.def')
    open(path, 'w').write(f'LIBRARY {name}.dll\nEXPORTS\n' + '\n'.join(lines) + '\n')
    return ['/def:' + path]

def image_size(dll):
    """a PE file's SizeOfImage"""
    import struct
    data = open(dll, 'rb').read(4096)
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    return struct.unpack_from('<I', data, pe + 24 + 56)[0]

def link_dll(odir, name, objs, deps, base, extra=(), entry=None):
    extra = list(extra) + ordinal_exports(name, objs)
    if arch() == 'x86':
        extra += x86_def(odir, name, objs) + ['/safeseh:no', '/machine:x86']
    dll = os.path.join(odir, f'{name}.dll')
    entry = entry or DLLS.get(name, {}).get('entry')
    entry = [f'/entry:{entry}'] if entry else ['/noentry']
    run(['lld-link', '/dll', '/nodefaultlib', f'/base:{base:#x}'] + entry +
        [f'/out:{dll}', f'/implib:{os.path.join(odir, name + ".lib")}', f'/map:{os.path.join(odir, name + ".map")}'] +
        objs + list(extra) +
        [os.path.join(odir, d + '.lib') for d in deps])
    sysdir = 'System32' if arch() == 'x64' else 'SysWOW64'
    built.append((f'\\Windows\\{sysdir}\\{name}.dll', dll))
    placed.append((base, base + image_size(dll), name))

def check_overlaps(pass_arch, spans):
    """no two DLLs of a pass (@spans: (start, end, name)) loaded over each
    other (each needs its own range: they are linked without relocation in mind)"""
    spans = sorted(spans)
    for (b0, e0, n0), (b1, e1, n1) in zip(spans, spans[1:]):
        if b1 < e0:
            raise SystemExit(f'{pass_arch}: {n0}.dll ({b0:#x}-{e0:#x}) overlaps {n1}.dll at {b1:#x}; '
                             f'move one (its dll.json base) or leave its base out')

class Collected:
    """the lists DLL build hooks append to (b.built, b.placed): while a build
    task runs, its appends go to the task's own lists (so concurrent DLLs
    cannot interleave); build_pass puts them in a fixed order afterwards"""
    def __init__(self, attr):
        self.attr, self.main = attr, []
    def cur(self):
        return getattr(_task, self.attr, self.main)
    def append(self, x): self.cur().append(x)
    def extend(self, xs): self.cur().extend(xs)
    def __iadd__(self, xs):
        self.extend(xs)
        return self
    def __getitem__(self, i): return self.cur()[i]
    def __setitem__(self, i, x): self.cur()[i] = x
    def __iter__(self): return iter(self.cur())
    def __len__(self): return len(self.cur())

def run_tasks(tasks):
    """run @tasks, {key: (keys it waits for, function)}, on JOBS threads,
    each once its dependencies are done (of the ready ones, the earliest in
    @tasks first); returns {key: result}.  The first failure stops starting
    new tasks and is raised once running ones end"""
    from concurrent.futures import ThreadPoolExecutor, wait, FIRST_COMPLETED
    results, running, error = {}, {}, None
    todo = dict(tasks)
    with ThreadPoolExecutor(max_workers=JOBS) as ex:
        while todo or running:
            if not error:       # (at most JOBS submitted: tasks that become ready later still go before the rest of todo)
                for key in [k for k, (deps, _) in todo.items() if all(d in results for d in deps)][:JOBS - len(running)]:
                    running[ex.submit(todo.pop(key)[1])] = key
            if not running:
                if todo and not error:
                    raise SystemExit('build tasks wait on each other: ' + ', '.join(sorted(todo)))
                break
            done, _ = wait(running, return_when=FIRST_COMPLETED)
            for f in done:
                key = running.pop(f)
                try:
                    results[key] = f.result()
                except BaseException as e:
                    error = error or e
    if error:
        raise error
    return results

def build_pass(arch, all_tasks):
    """add the DLLs and programs for one architecture to @all_tasks (keys
    "ARCH:name"); returns the function that, given all the tasks' results,
    records what the pass built"""
    odir = out if arch == 'x64' else os.path.join(out, 'x86')
    os.makedirs(odir, exist_ok=True)
    me = sys.modules[__name__]
    tasks = {}                      # key: (keys it waits for, function)
    # (a task's appends to built/placed are its own: see Collected)
    def task(key, deps, fn):
        def go():
            _task.arch, _task.built, _task.placed = arch, [], []
            r = fn()
            return r, _task.built, _task.placed
        tasks[key] = (deps, go)

    # startup code + implicit-TLS support (needed by DLLs and programs)
    crt0 = os.path.join(odir, 'crt0.obj')
    tlssup = os.path.join(odir, 'tlssup.obj')
    rt = os.path.join(odir, 'x86rt.obj')
    task('crt0', [], lambda: cc(os.path.join(HERE, 'crt', 'crt0.c'), crt0))
    task('tlssup', [], lambda: cc(os.path.join(HERE, 'lib', 'tlssup.c'), tlssup))
    prelude = ['crt0', 'tlssup']
    if arch == 'x86':                              # 64-bit division helpers the compiler calls
        task('x86rt', [], lambda: cc(os.path.join(HERE, 'lib', 'x86rt.c'), rt))
        prelude.append('x86rt')
        crt0_objs = [crt0, rt]
    else:
        crt0_objs = [crt0]

    # system DLLs (userland/*/dll.json), each after its dependencies
    def build_dll(name, m):
        base = m['base'] if arch == 'x64' else m['base_x86']
        deps = m.get('deps', [])
        objs = dll_objs(odir, name, m.get('sources', [name]))
        if hook(name, 'objs'):
            objs += hook(name, 'objs')(me, odir)
        if m.get('tlssup'):
            objs.append(tlssup)
        if arch == 'x86':
            objs.append(rt)
        rc = os.path.join(HERE, m.get('sources', [name])[0], name + '.rc')   # NAME.rc: resources (testdll's type library)
        if os.path.exists(rc):
            res = os.path.join(odir, name + '.res')
            run([build_netsurf.llvm_rc()] + (['/D', 'NOVA_X86'] if arch == 'x86' else []) + ['/FO', res, rc])
            objs.append(res)
        if hook(name, 'link'):
            hook(name, 'link')(me, odir, objs, deps, base)
        else:
            link_dll(odir, name, objs, deps, base)
    dlls = [n for n, m in DLLS.items() if not (arch == 'x86' and m.get('x64_only'))]
    for name in dlls:
        task('dll:' + name, prelude + ['dll:' + d for d in DLLS[name].get('deps', [])],
             lambda name=name: build_dll(name, DLLS[name]))

    # programs (userland/programs/NAME.c or .cpp, settings in NAME.json)
    progdir = os.path.join(HERE, 'programs')
    progs = []
    for src in sorted(os.listdir(progdir)):
        if not src.endswith(('.c', '.cpp')):
            continue
        name = src.rsplit('.', 1)[0]
        prog = PROGRAMS.get(name, {})
        if arch == 'x86' and not prog.get('x86'):
            continue
        progs.append((src, name, prog))
    stl_libs = ('msvcp140', 'msvcp140_1', 'msvcp140_2', 'msvcp140_atomic_wait')
    compiled, stlobjs = {}, []     # each program's object and resources; what msstl programs link
    if any(src.endswith('.cpp') and prog.get('msstl') for src, name, prog in progs):
        task('stlprog', [], lambda: stlobjs.extend(HOOKS['msvcp140'].program_objs(me, odir)))
    def compile_prog(src, name, prog):
        obj = os.path.join(odir, f'prog_{name}.obj')
        if src.endswith('.cpp') and prog.get('msstl'):  # C++ on the standard library (msvcp140)
            run(['clang++'] + HOOKS['msvcp140'].program_flags(me) + ['-c', os.path.join(progdir, src), '-o', obj])
        elif src.endswith('.cpp'):                # C++ (exceptions, RTTI): vcruntime140
            run(['clang++'] + cflags() + ['-fcxx-exceptions', '-fexceptions', '-std=c++17',
                 '-c', os.path.join(progdir, src), '-o', obj])
        else:
            cc(os.path.join(progdir, src), obj)
        res = []                                  # NAME.rc: resources (e.g. the icon)
        rc = os.path.join(progdir, name + '.rc')
        if os.path.exists(rc):
            res = [os.path.join(odir, f'prog_{name}.res')]
            run([build_netsurf.llvm_rc(), '/FO', res[0], rc])
        compiled[name] = obj, res
    def link_prog(src, name, prog):
        exe = os.path.join(odir, f'{name}.exe')
        obj, res = compiled[name]
        stl = (stlobjs + [os.path.join(odir, l + '.lib') for l in stl_libs]) \
            if src.endswith('.cpp') and prog.get('msstl') else []
        libs = PROGRAM_LIBS + [l for l in prog.get('libs', []) if l not in PROGRAM_LIBS]
        run(['lld-link', '/subsystem:console', '/entry:mainCRTStartup', '/nodefaultlib'] +
            (['/safeseh:no', '/machine:x86'] if arch == 'x86' else []) +
            [f'/out:{exe}'] + crt0_objs + [tlssup, obj] + stl + res + [os.path.join(odir, l + '.lib') for l in libs])
        if arch == 'x86':
            folder = '\\Windows\\SysWOW64' if prog.get('system') else '\\Programs\\x86'
        else:
            folder = '\\Windows\\System32' if prog.get('system') else '\\Programs'
        built.append((f'{folder}\\{name}.exe', exe))
    for src, name, prog in progs:
        task('cc:' + name, [], lambda a=(src, name, prog): compile_prog(*a))
        msstl = src.endswith('.cpp') and prog.get('msstl')
        libs = PROGRAM_LIBS + [l for l in prog.get('libs', []) if l not in PROGRAM_LIBS] + (list(stl_libs) if msstl else [])
        deps = ['cc:' + name] + prelude + ['dll:' + l for l in libs if 'dll:' + l in tasks] + \
               (['stlprog'] if msstl else [])
        task('exe:' + name, deps, lambda a=(src, name, prog): link_prog(*a))

    for key, (deps, fn) in tasks.items():
        all_tasks[f'{arch}:{key}'] = ([f'{arch}:{d}' for d in deps], fn)

    def record(results):
        """what the pass built, in a fixed order: the DLLs in link order,
        the programs by name"""
        spans = []
        for key in [f'dll:{n}' for n in dlls] + [f'exe:{n}' for _, n, _ in progs]:
            _, b, p = results[f'{arch}:{key}']
            built.main.extend(b)
            spans.extend(p)
        check_overlaps(arch, spans)
    return record

placed = Collected('placed')      # (start, end, name) of each DLL linked by the running task
built = Collected('built')
passes = ['x64'] + (['x86'] if os.environ.get('NOVA_NO_WOW64') != '1' else [])   # x86: 32-bit programs, the same userland
all_tasks = {}
records = [build_pass(a, all_tasks) for a in passes]

# The NetSurf browser (tools/build_netsurf.py) compiles beside the passes
# (it needs only headers); its link waits for the x64 start-up objects and
# the import libraries of the DLLs it links.
if os.environ.get('NOVA_NO_NETSURF') != '1':
    ns_out = os.path.join(out, 'netsurf')
    ns_jobs, ns_objs = build_netsurf.plan(ns_out)
    ns_errors = []
    def ns_compile(job):
        _task.arch = 'netsurf'
        with SLOTS:
            ns_errors.append(build_netsurf.compile_one(job))
    for i, job in enumerate(ns_jobs):
        all_tasks[f'netsurf:cc:{i}'] = ([], lambda job=job: ns_compile(job))
    def ns_link():
        _task.arch = 'netsurf'
        build_netsurf.report(ns_errors)
        with SLOTS:
            return build_netsurf.link(ns_out, out, ns_objs)
    all_tasks['netsurf:link'] = (
        [f'netsurf:cc:{i}' for i in range(len(ns_jobs))] + ['x64:crt0', 'x64:tlssup'] +
        [f'x64:dll:{n}' for n in ('msvcrt', 'kernel32', 'ntdll', 'ws2_32', 'user32', 'gdi32')], ns_link)

results = run_tasks(all_tasks)    # (the x64 pass first: its tasks start first)
for record in records:
    record(results)

# 3a0. fonts gdi32 draws text with (C:\Windows\Fonts)
TP = os.path.join(os.path.dirname(HERE), 'third_party')
for src, dst in [('inter/Inter-Regular.ttf', 'inter.ttf'), ('inter/Inter-Bold.ttf', 'interbd.ttf'),
                 ('dejavu/DejaVuSansMono.ttf', 'dejavumono.ttf'), ('dejavu/DejaVuSansMono-Bold.ttf', 'dejavumonobd.ttf'),
                 ('noto/NotoSansArabic-Regular.ttf', 'notosansarabic.ttf'),
                 ('noto/NotoSansDevanagari-Regular.ttf', 'notosansdevanagari.ttf')]:
    built.append((f'\\Windows\\Fonts\\{dst}', os.path.join(TP, src)))

# 3a0b. ICU, as Windows 10 ships it (tools/build_icu.py builds third_party/icu):
# one icu.dll per architecture with ICU's C API under unversioned names, and
# the data both read from C:\Windows\Globalization\ICU.  .NET formats,
# compares and names cultures through it when icu.dll loads.
ICU = os.path.join(TP, 'icu')
built.append(('\\Windows\\System32\\icu.dll', os.path.join(ICU, 'x64', 'icu.dll')))
if os.environ.get('NOVA_NO_WOW64') != '1':
    built.append(('\\Windows\\SysWOW64\\icu.dll', os.path.join(ICU, 'x86', 'icu.dll')))
built.append(('\\Windows\\Globalization\\ICU\\icudt77l.dat', os.path.join(ICU, 'icudt77l.dat')))

# 3a1. the trusted roots secur32's Schannel checks certificates against
# (the kernel's Mozilla list, as DER certificates back to back)
roots = os.path.join(out, 'ca-bundle.der')
build_netsurf.root_bundle(roots)
built.append(('\\Windows\\System32\\ca-bundle.der', roots))

# 3a2. the Windows Installer packages the msitest self-test installs
# (tools/msitest/mkpkg.py writes them; their programs are copies of msitest)
msipkg = os.path.join(out, 'msitest')
run([sys.executable, os.path.join(os.path.dirname(HERE), 'tools', 'msitest', 'mkpkg.py'), msipkg,
     os.path.join(out, 'msitest.exe')])
for n in sorted(os.listdir(msipkg)):
    built.append((f'\\Tests\\Msi\\{n}', os.path.join(msipkg, n)))
# 3a3. the General MIDI soundfont winmm's synthesizer plays (generated)
sf2 = os.path.join(out, 'gm.sf2')
run([sys.executable, os.path.join(os.path.dirname(HERE), 'tools', 'make_gm_soundfont.py'), sf2])
built.append(('\\Windows\\System32\\drivers\\gm.sf2', sf2))
built.append(('\\Windows\\SysWOW64\\drivers\\gm.sf2', sf2))     # (where WOW64 redirects 32-bit programs)

# 3a4. the audio DSP's Sound Open Firmware, when tools/fetch_sof_firmware.py
# fetched it (it is not in the repository; kernel/drivers/sof.c loads it)
SOF = os.path.join(TP, 'sof-bin')
if os.path.isdir(SOF):
    built.append(('\\Windows\\Firmware\\Intel\\LICENCE.Intel', os.path.join(SOF, 'LICENCE.Intel')))
    for plat in sorted(os.listdir(os.path.join(SOF, 'sof-ipc4'))):
        built.append((f'\\Windows\\Firmware\\Intel\\sof-ipc4\\{plat}\\sof-{plat}.ri',
                      os.path.join(SOF, 'sof-ipc4', plat, f'sof-{plat}.ri')))

# 3a. sample files for the user's folders (tools/make_icons.py draws the icons)
samples = os.path.join(HERE, 'samples')
for n in sorted(os.listdir(samples)):
    built.append((f'\\Pictures\\{n}', os.path.join(samples, n)))

# 3b. the NetSurf web browser
if os.environ.get('NOVA_NO_NETSURF') != '1':
    built += results['netsurf:link']

# 4. embed (.incbin: the browser alone is megabytes, too much for C arrays).
# Files of 1 MiB or more (ICU's data and DLLs, NetSurf) go in zlib-compressed
# and the kernel inflates them at boot: the kernel image stays small.
import zlib
zdir = os.path.join(out, 'z')
os.makedirs(zdir, exist_ok=True)
stored = []                     # (path, file embedded, size, compressed size or 0)
for i, (path, file) in enumerate(built):
    size = os.path.getsize(file)
    if size < (1 << 20):
        stored.append((path, file, size, 0))
        continue
    data = open(file, 'rb').read()
    z = os.path.join(zdir, f'{i}.z')
    packed = zlib.compress(data, 9)
    if not os.path.exists(z) or open(z, 'rb').read() != packed:
        open(z, 'wb').write(packed)
    stored.append((path, z, size, len(packed)))
with open(gen_c + '.tmp', 'w') as f:
    f.write('/* generated by tools/build_userland.py — the Windows userland on drive C: */\n')
    f.write('#include "um/userland_files.h"\n\n')
    for i, (path, file, size, zsize) in enumerate(stored):
        src = os.path.abspath(file).replace('\\', '\\\\').replace('"', '\\"')
        f.write(f'extern const unsigned char nova_userland_{i}[];\n')
        f.write(f'__asm__(".section .rodata.userland,\\"a\\"\\n.balign 16\\n'
                f'.globl nova_userland_{i}\\nnova_userland_{i}:\\n.incbin \\"{src}\\"\\n.previous");\n')
    f.write('\nconst UserlandFile g_userland_files[] = {\n')
    for i, (path, file, size, zsize) in enumerate(stored):
        cpath = path.replace('\\', '\\\\')
        f.write(f'    {{ "{cpath}", nova_userland_{i}, {size}, {zsize} }},\n')
    f.write('};\n')
    f.write(f'const int g_userland_file_count = {len(built)};\n')
os.replace(gen_c + '.tmp', gen_c)
print(f'userland: {len(built)} files')
