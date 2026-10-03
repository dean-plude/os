#!/usr/bin/env python3
"""Build the generated parts of README.md, docs/ROADMAP.md,
docs/HISTORY.md and docs/building.md from one-file-per-item fragments.

    tools/docgen.py                 rewrite the generated regions in place
    tools/docgen.py --check         exit 1 if a region is out of date
    tools/docgen.py --check-pr REV  exit 1 if this tree's regions differ from
                                    REV's (a change edited generated text by
                                    hand instead of adding or editing a
                                    fragment)
    tools/docgen.py --list          print each region and its sources

Parallel pull requests used to collide on the same lines of these files
(every change appended to the same list).  Now each item is its own file:

  docs/history/*.md            HISTORY.md's sections, one per feature
  docs/roadmap/<area>/*.md     ROADMAP.md's "What comes next" items
  docs/readme/programs/*.md    README's "Unmodified Windows programs" rows
  docs/readme/inside/*.md      README's "What is inside" items
  docs/readme/licenses/*.md    README's list of bundled licences
  docs/selftests/*.md          docs/building.md's self-test program table
  tests/selftest/core/*.py     README's core self-test list (their DOC)
  tests/appcorpus/*.py         README's nightly app list (their DOC)
  userland/programs/*.json     README's self-test programs ("selftest")

Fragments are taken in file-name order (a number or date prefix places
them).  A region sits between "<!-- BEGIN generated:NAME -->" and
"<!-- END generated:NAME -->" in the target file; everything outside the
markers is ordinary text, edited as usual.  Pull requests add or edit
fragments and leave the regions alone; after a merge to main the docs
workflow (.github/workflows/docs.yml) runs this script and commits the
result.  See CONTRIBUTING.md.
"""
import ast, glob, json, os, re, subprocess, sys, textwrap

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MARK = re.compile(r'(<!-- BEGIN generated:([\w-]+) -->)(.*?)(<!-- END generated:\2 -->)', re.S)


def fragments(d, ext='.md'):
    """the files in @d (relative to the repository), in name order"""
    return sorted(glob.glob(os.path.join(ROOT, d, '*' + ext)))


def read(f):
    return open(f, encoding='utf-8').read()


def framed(text):
    """a block region: the markers on lines of their own, blank lines
    around the text"""
    return '\n\n' + text + '\n'


def blocks(d, sep='\n'):
    """@d's fragments, each ending in one newline, joined by @sep"""
    return framed(sep.join(read(f).strip('\n') + '\n' for f in fragments(d)))


def inline(d, sep):
    """@d's fragments as one run of text (each file one item)"""
    return sep.join(' '.join(read(f).split()) for f in fragments(d))


def py_doc(f):
    """the DOC string a test-registry file defines (without running it)"""
    for node in ast.parse(read(f)).body:
        if isinstance(node, ast.Assign) and any(getattr(t, 'id', None) == 'DOC' for t in node.targets):
            return ast.literal_eval(node.value)
    return None


def table(head, d):
    """a Markdown table: @head's columns, one row per fragment of @d"""
    cols = '| ' + ' | '.join(head) + ' |\n|' + '---|' * len(head) + '\n'
    return framed(cols + ''.join(read(f).strip('\n') + '\n' for f in fragments(d)))


def core_tests():
    """the core suite in order, as one sentence (it sits inside a list item:
    continuation lines are indented)"""
    docs = [d for d in (py_doc(f) for f in fragments('tests/selftest/core', '.py')) if d]
    text = ', '.join(docs[:-1]) + ', and last ' + docs[-1] + '.'
    return textwrap.fill(text, 72, subsequent_indent='  ', break_on_hyphens=False, break_long_words=False)


def selftest_programs():
    names = []
    for f in fragments('userland/programs', '.json'):
        if json.loads(read(f)).get('selftest'):
            names.append(os.path.basename(f)[:-5])
    return ', '.join(f'`{n}`' for n in names)


def corpus_apps():
    docs = [d for d in (py_doc(f) for f in fragments('tests/appcorpus', '.py')) if d]
    return ', '.join(docs[:-1]) + ' and ' + docs[-1] if len(docs) > 1 else ''.join(docs)


ROADMAP_AREAS = {'next-graphics': 'graphics', 'next-apps': 'apps', 'next-kernel': 'kernel',
                 'next-hardware': 'hardware'}

REGIONS = {
    'README.md': {
        'programs': (lambda: table(['Program', 'Kind', 'Tested on NovaOS'], 'docs/readme/programs'),
                     'docs/readme/programs/*.md'),
        'inside': (lambda: blocks('docs/readme/inside', ''), 'docs/readme/inside/*.md'),
        'core-tests': (core_tests, 'tests/selftest/core/*.py (DOC)'),
        'selftest-programs': (selftest_programs, 'userland/programs/*.json ("selftest": true)'),
        'corpus': (corpus_apps, 'tests/appcorpus/*.py (DOC)'),
        'licenses': (lambda: inline('docs/readme/licenses', '; '), 'docs/readme/licenses/*.md'),
    },
    'docs/ROADMAP.md': {
        name: ((lambda a=area: blocks(f'docs/roadmap/{a}', '')), f'docs/roadmap/{area}/*.md')
        for name, area in ROADMAP_AREAS.items()
    },
    'docs/building.md': {
        'selftest-table': (lambda: table(['Program', 'Covers'], 'docs/selftests'), 'docs/selftests/*.md'),
    },
    'docs/HISTORY.md': {
        'history': (lambda: blocks('docs/history'), 'docs/history/*.md'),
    },
}


def render(target, text):
    """@text with every region of @target rebuilt"""
    regions = REGIONS[target]
    seen = set()

    def one(m):
        name = m.group(2)
        if name not in regions:
            sys.exit(f'{target}: unknown generated region "{name}"')
        seen.add(name)
        body = regions[name][0]()
        return m.group(1) + body + m.group(4)
    out = MARK.sub(one, text)
    missing = set(regions) - seen
    if missing:
        sys.exit(f'{target}: no markers for region(s) {", ".join(sorted(missing))}')
    return out


def regions_of(text):
    return {m.group(2): m.group(3) for m in MARK.finditer(text)}


def git_show(rev, path):
    r = subprocess.run(['git', '-C', ROOT, 'show', f'{rev}:{path}'], capture_output=True, text=True)
    return r.stdout if r.returncode == 0 else None


def main():
    args = sys.argv[1:]
    if args[:1] == ['--list']:
        for target, regions in REGIONS.items():
            for name, (_, src) in regions.items():
                print(f'{target:18s} {name:18s} <- {src}')
        return 0
    if args[:1] == ['--check-pr']:
        rev = args[1] if len(args) > 1 else 'HEAD^1'
        bad = 0
        for target in REGIONS:
            mine = read(os.path.join(ROOT, target))
            render(target, mine)                # the fragments build
            old = git_show(rev, target)
            if old is None:
                continue
            a, b = regions_of(old), regions_of(mine)
            for name in sorted(set(a) & set(b)):  # (a change adding or retiring a region is exempt)
                if a[name] != b[name]:
                    src = REGIONS[target].get(name, (None, '?'))[1]
                    print(f'::error file={target}::{target}: the generated region "{name}" was edited by hand. '
                          f'Put the change in {src} instead and keep {target} as on main; '
                          f'tools/docgen.py rebuilds it after the merge (CONTRIBUTING.md).')
                    bad += 1
        if not bad:
            print('generated regions untouched; fragments build: OK')
        return 1 if bad else 0
    stale = 0
    for target in REGIONS:
        path = os.path.join(ROOT, target)
        text = read(path)
        new = render(target, text)
        if new != text:
            stale += 1
            if args[:1] == ['--check']:
                print(f'{target}: out of date (run tools/docgen.py)')
            else:
                open(path, 'w', encoding='utf-8').write(new)
                print(f'{target}: rebuilt')
    return 1 if stale and args[:1] == ['--check'] else 0


if __name__ == '__main__':
    sys.exit(main())
