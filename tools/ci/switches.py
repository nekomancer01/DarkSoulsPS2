"""The switch registry matches the sources; CI job "hygiene". Two tables make up the registry:
  ps2/proto/src/switches.h  engine switches: a name a #if / #ifdef / #ifndef / #elif in ps2/proto/src tests that the
                            sources do not #define themselves (or #define only as a default under #ifndef NAME)
  tools/switches.tsv        build-time switches: every ?= variable of ps2/proto/Makefile and every variable it reads
                            but never sets (scope make), and every environment variable tools/ reads (scope env)
Every row has an owner (a Polaris module, OWNERS below) and the PR that added the switch (#N, "init" for the first
public release, c:<commit> for a direct commit,
new in the PR that adds it until its number is known). Fails when a switch has no row, when a row names something nothing
reads any more, when a row's kind or owner is unknown or its PR is malformed, or when a CI column sets a name with no
row. Shell scripts' variables (tools/*.sh) are run settings, not switches, and are not listed.
  --list   print "NAME<TAB>compiler args" per CI build (what tools/ci/switches.sh builds), nothing else
  --tsv    print the whole registry as one table: scope, name, default, kind, owner, off, pr, where, what
           (scope c = switches.h; off = the build or value that selects the old / other path)
Usage: python3 -I tools/ci/switches.py [--list | --tsv]"""
import glob
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
SRC = os.path.join(ROOT, 'ps2', 'proto', 'src')
HEADER = os.path.join(SRC, 'switches.h')
MAKEFILE = os.path.join(ROOT, 'ps2', 'proto', 'Makefile')
# the export knobs and rules ps2/proto/Makefile includes, generated from tools/recipe.toml (tools/recipe.py make)
MAKE_PARTS = [MAKEFILE] + [os.path.join(ROOT, 'ps2', 'proto', f) for f in ('export_vars.mk', 'export_rules.mk')]
TOOLS = os.path.join(ROOT, 'tools')
BUILD_TSV = os.path.join(TOOLS, 'switches.tsv')
KINDS = {'fallback', 'debug', 'tune', 'make', 'host'}
BUILD_KINDS = {'flag', 'tune', 'debug', 'opt', 'config'}
BUILD_COLS = ['name', 'scope', 'default', 'kind', 'owner', 'off', 'pr', 'where', 'what']
# Polaris layers (polaris plan section 4) plus tools for the host side; "main" is the frame loop until it splits
OWNERS = {'hal', 'core', 'io', 'gfx', 'world', 'scene', 'game', 'ui', 'snd', 'fx', 'cine', 'main', 'tools'}
PR = re.compile(r'^(#\d+|new|init|c:[0-9a-f]{7,40})$')
# make's own variables and the ps2sdk Makefile.pref ones the recipe reads without setting
MAKE_BUILTIN = {'MAKECMDGOALS', 'CURDIR', 'MAKE', 'SHELL', 'MAKEFLAGS', 'MAKEFILE_LIST'}
ROW = re.compile(r'^ \* ([A-Z][A-Z0-9_]*)\s*\|(.*)$')


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def tested_names():
    """name -> files whose preprocessor conditions test it, for names the sources do not define outright."""
    tested, outright, guarded_default = {}, set(), set()
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
                else:
                    guarded_default.add(m.group(1))
    # a name with a default under #ifndef NAME is a switch even where another #define overrides it (e.g. a debug
    # build turning the new path off)
    return {k: v for k, v in tested.items() if k not in outright or k in guarded_default}


def rows(full=False):
    """[(name, default, kind, [ci arg strings], where, what, line)] from switches.h; full: (..., owner, pr, ci) too."""
    out = []
    for n, line in enumerate(open(HEADER, encoding='utf-8'), 1):
        m = ROW.match(line.rstrip('\n'))
        if not m:
            continue
        cols = [c.strip() for c in m.group(2).split('|')]
        if len(cols) != 7:
            sys.exit(f'switches.h:{n}: {m.group(1)}: expected 8 columns separated by | (name | default | kind | CI | '
                     f'owner | pr | where | what), got {len(cols) + 1}')
        default, kind, ci, owner, pr, where, what = cols
        builds = [] if ci in ('-', '') or ci.startswith('(') else [b.strip() for b in ci.split(';') if b.strip()]
        r = (m.group(1), default, kind, builds, where, what, n)
        out.append(r + (owner, pr, ci) if full else r)
    return out


def make_names():
    """Makefile switches: name -> where ('?=' for an overridable default, 'read' for a variable read but never set)."""
    text = re.sub(r'(?m)(?<!\\)#.*$', '', '\n'.join(open(p, encoding='utf-8').read() for p in MAKE_PARTS
                                                     if os.path.exists(p)))
    assigned = set(re.findall(r'^\s*(?:override\s+|export\s+)?([A-Za-z_]\w*)\s*(?:\?|:|\+|::)?=', text, re.M))
    soft = set(re.findall(r'^\s*([A-Za-z_]\w*)\s*\?=', text, re.M))
    used = set(re.findall(r'\$[({]([A-Z_][A-Z0-9_]*)[)}]', text)) | set(re.findall(r'^\s*if(?:n?def)\s+([A-Z_]\w*)', text, re.M))
    out = {n: '?=' for n in soft}
    for n in used - assigned - MAKE_BUILTIN:
        out[n] = 'read'
    return out


ENV_READ = re.compile(r'''os\.environ\.get\(\s*['"]([A-Z][A-Z0-9_]*)['"]|os\.environ\[\s*['"]([A-Z][A-Z0-9_]*)['"]\s*\]|'''
                      r'''os\.getenv\(\s*['"]([A-Z][A-Z0-9_]*)['"]|\bgetenv\(\s*"([A-Z][A-Z0-9_]*)"''')


def tool_files():
    """tools/ files that may read the environment: the tracked ones (glob when git is missing)"""
    try:
        out = subprocess.run(['git', 'ls-files', '--', '*.py', '*.c', '*.h'], cwd=TOOLS, capture_output=True,
                             text=True, check=True).stdout.split()
        return [os.path.join(TOOLS, p) for p in out]
    except (OSError, subprocess.CalledProcessError):
        return sorted(p for ext in ('py', 'c', 'h') for p in glob.glob(os.path.join(TOOLS, '**', '*.' + ext),
                                                                      recursive=True))


def env_names():
    """Environment variables tools/ reads (Python, and the native helpers' getenv): name -> [files]."""
    out = {}
    for path in tool_files():
        rel = os.path.relpath(path, TOOLS)
        for m in ENV_READ.finditer(open(path, encoding='utf-8', errors='replace').read()):
            name = next(g for g in m.groups() if g)
            files = out.setdefault(name, [])
            if rel not in files:
                files.append(rel)
    return out


def build_rows():
    """[dict per tools/switches.tsv row, with 'line']"""
    out = []
    with open(BUILD_TSV, encoding='utf-8') as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip('\n')
            if not line or line.startswith('#'):
                continue
            cols = line.split('\t')
            if cols == BUILD_COLS:
                continue
            if len(cols) != len(BUILD_COLS):
                sys.exit(f'switches.tsv:{n}: expected {len(BUILD_COLS)} tab-separated columns '
                         f'({" ".join(BUILD_COLS)}), got {len(cols)}')
            r = dict(zip(BUILD_COLS, cols))
            r['line'] = n
            out.append(r)
    return out


def check_build(errors):
    table = build_rows()
    seen = {}
    for r in table:
        key = (r['scope'], r['name'])
        if key in seen:
            errors.append(f'switches.tsv:{r["line"]}: {r["scope"]} {r["name"]} listed twice (also line {seen[key]})')
        seen[key] = r['line']
        if r['scope'] not in ('make', 'env'):
            errors.append(f'switches.tsv:{r["line"]}: {r["name"]}: scope "{r["scope"]}" is not make or env')
        if r['kind'] not in BUILD_KINDS:
            errors.append(f'switches.tsv:{r["line"]}: {r["name"]}: kind "{r["kind"]}" is not one of '
                          f'{", ".join(sorted(BUILD_KINDS))}')
        if r['owner'] not in OWNERS:
            errors.append(f'switches.tsv:{r["line"]}: {r["name"]}: owner "{r["owner"]}" is not one of '
                          f'{", ".join(sorted(OWNERS))}')
        if not PR.match(r['pr']):
            errors.append(f'switches.tsv:{r["line"]}: {r["name"]}: pr "{r["pr"]}" is not #N, new, init or c:<commit>')
    for scope, found, what in (('make', make_names(), 'ps2/proto/Makefile'), ('env', env_names(), 'tools/')):
        for name in sorted(set(found) - {n for s, n in seen if s == scope}):
            where = found[name] if scope == 'make' else ' '.join(found[name])
            errors.append(f'{what}: {name} ({where}) has no {scope} row in tools/switches.tsv '
                          '(add it: name, scope, default, kind, owner, off, pr, where, what)')
        for name in sorted({n for s, n in seen if s == scope} - set(found)):
            errors.append(f'switches.tsv:{seen[(scope, name)]}: {scope} {name}: nothing in {what} reads it any more '
                          '(remove the row)')
    return len(table)


def main():
    table = rows()
    if '--list' in sys.argv[1:]:
        for name, _, _, builds, *_ in table:
            for b in builds:
                print(f'{name}\t{b}')
        return 0
    if '--tsv' in sys.argv[1:]:
        print('\t'.join(['scope'] + [c for c in BUILD_COLS if c != 'scope']))
        for name, default, kind, _, where, what, _, owner, pr, ci in rows(full=True):
            print('\t'.join(['c', name, default, kind, owner, ci, pr, where, what]))
        for r in build_rows():
            print('\t'.join([r['scope']] + [r[c] for c in BUILD_COLS if c != 'scope']))
        return 0
    errors = []
    names = {}
    for name, default, kind, builds, where, what, n, owner, pr, _ in rows(full=True):
        if owner not in OWNERS:
            errors.append(f'switches.h:{n}: {name}: owner "{owner}" is not one of {", ".join(sorted(OWNERS))}')
        if not PR.match(pr):
            errors.append(f'switches.h:{n}: {name}: pr "{pr}" is not #N, new, init or c:<commit>')
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
                      '(add it: default, kind, CI build, owner, pr, where, what)')
    for name in sorted(set(names) - set(tested)):
        errors.append(f'switches.h:{names[name]}: {name}: no source tests it any more (remove the row)')
    nbuild = check_build(errors)
    for e in errors:
        print('FAIL', e)
    print(f'switches: {len(table)} engine rows, {nbuild} build-time rows, {sum(len(r[3]) for r in table)} CI builds, '
          f'{len(errors)} problems')
    return 1 if errors else 0


def line_ci(n):
    line = open(HEADER, encoding='utf-8').read().split('\n')[n - 1]
    return line.split('|')[3] if line.count('|') >= 4 else ''


if __name__ == '__main__':
    sys.exit(main())
