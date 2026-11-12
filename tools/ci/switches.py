"""The compile-time switch list (ps2/proto/src/switches.h) matches the sources; CI job "hygiene".
A switch is a name a #if / #ifdef / #ifndef / #elif in ps2/proto/src tests that the sources do not #define themselves
(or #define only as a default under #ifndef NAME). Fails when a switch has no row in switches.h, when a row names
something no source tests any more, when a row's kind is unknown, or when its CI column sets a name with no row.
  --list   print "NAME<TAB>compiler args" per CI build (what tools/ci/switches.sh builds), nothing else
Usage: python3 -I tools/ci/switches.py [--list]"""
import glob
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
SRC = os.path.join(ROOT, 'ps2', 'proto', 'src')
HEADER = os.path.join(SRC, 'switches.h')
KINDS = {'fallback', 'debug', 'tune', 'make', 'host'}
ROW = re.compile(r'^ \* ([A-Z][A-Z0-9_]*)\s*\|(.*)$')


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def tested_names():
    """name -> files whose preprocessor conditions test it, for names the sources do not define outright."""
    tested, outright = {}, set()
    for path in sorted(glob.glob(os.path.join(SRC, '*.[ch]'))):
        name = os.path.basename(path)
        if path == HEADER:
            continue
        lines = strip_comments(open(path, encoding='utf-8', errors='replace').read()).split('\n')
        for i, line in enumerate(lines):
            m = re.match(r'\s*#\s*(?:ifdef|ifndef|if|elif)\b(.*)', line)
            if m:
                for tok in re.findall(r'\b[A-Za-z_]\w*\b', m.group(1)):
                    if tok != 'defined' and not tok.startswith('_'):
                        tested.setdefault(tok, set()).add(name)
            m = re.match(r'\s*#\s*define\s+(\w+)(.*)', line)
            if m:
                prev = lines[i - 1] if i else ''
                guarded = re.match(r'\s*#\s*ifndef\s+' + m.group(1) + r'\b', prev)
                # (a header's include guard: #ifndef X / #define X with no value)
                if not guarded or (name.endswith('.h') and not m.group(2).strip()):
                    outright.add(m.group(1))
    return {k: v for k, v in tested.items() if k not in outright}


def rows():
    """[(name, default, kind, [ci arg strings], where, what)] from switches.h."""
    out = []
    for n, line in enumerate(open(HEADER, encoding='utf-8'), 1):
        m = ROW.match(line.rstrip('\n'))
        if not m:
            continue
        cols = [c.strip() for c in m.group(2).split('|')]
        if len(cols) != 5:
            sys.exit(f'switches.h:{n}: {m.group(1)}: expected 6 columns separated by |, got {len(cols) + 1}')
        default, kind, ci, where, what = cols
        builds = [] if ci in ('-', '') or ci.startswith('(') else [b.strip() for b in ci.split(';') if b.strip()]
        out.append((m.group(1), default, kind, builds, where, what, n))
    return out


def main():
    table = rows()
    if '--list' in sys.argv[1:]:
        for name, _, _, builds, *_ in table:
            for b in builds:
                print(f'{name}\t{b}')
        return 0
    errors = []
    names = {}
    for name, default, kind, builds, where, what, n in table:
        if name in names:
            errors.append(f'switches.h:{n}: {name} listed twice (also line {names[name]})')
        names[name] = n
        if kind not in KINDS:
            errors.append(f'switches.h:{n}: {name}: kind "{kind}" is not one of {", ".join(sorted(KINDS))}')
        if kind in ('fallback', 'debug') and not builds and not re.search(r'\(with ', line_ci(n)):
            errors.append(f'switches.h:{n}: {name}: a {kind} switch needs a CI build (or "(with OTHER)")')
    for name, _, _, builds, _, _, n in table:
        for b in builds:
            for d in re.findall(r'-D([A-Za-z_]\w*)', b):
                if d not in names:
                    errors.append(f'switches.h:{n}: {name}: CI build sets {d}, which has no row')
    tested = tested_names()
    for name in sorted(set(tested) - set(names)):
        errors.append(f'{", ".join(sorted(tested[name]))}: tests {name}, which has no row in src/switches.h '
                      '(add it: default, kind, CI build, where, what)')
    for name in sorted(set(names) - set(tested)):
        errors.append(f'switches.h:{names[name]}: {name}: no source tests it any more (remove the row)')
    for e in errors:
        print('FAIL', e)
    print(f'switches: {len(table)} rows, {sum(len(r[3]) for r in table)} CI builds, {len(errors)} problems')
    return 1 if errors else 0


def line_ci(n):
    line = open(HEADER, encoding='utf-8').read().split('\n')[n - 1]
    return line.split('|')[3] if line.count('|') >= 4 else ''


if __name__ == '__main__':
    sys.exit(main())
